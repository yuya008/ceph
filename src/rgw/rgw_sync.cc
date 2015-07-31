#include "common/ceph_json.h"
#include "common/RWLock.h"
#include "common/RefCountedObj.h"

#include "rgw_common.h"
#include "rgw_rados.h"
#include "rgw_sync.h"
#include "rgw_metadata.h"
#include "rgw_rest_conn.h"


#define dout_subsys ceph_subsys_rgw

static string mdlog_sync_status_oid = "mdlog.sync-status";

void rgw_mdlog_info::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("num_objects", num_shards, obj);
}

struct rgw_mdlog_entry {
  string id;
  string section;
  string name;
  utime_t timestamp;
  RGWMetadataLogData log_data;

  void decode_json(JSONObj *obj);
};

struct rgw_mdlog_shard_data {
  string marker;
  bool truncated;
  vector<rgw_mdlog_entry> entries;

  void decode_json(JSONObj *obj);
};


void rgw_mdlog_entry::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("id", id, obj);
  JSONDecoder::decode_json("section", section, obj);
  JSONDecoder::decode_json("name", name, obj);
  JSONDecoder::decode_json("timestamp", timestamp, obj);
  JSONDecoder::decode_json("data", log_data, obj);
}

void rgw_mdlog_shard_data::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("marker", marker, obj);
  JSONDecoder::decode_json("truncated", truncated, obj);
  JSONDecoder::decode_json("entries", entries, obj);
};

int RGWRemoteMetaLog::init()
{
  conn = store->rest_master_conn;

  rgw_http_param_pair pairs[] = { { "type", "metadata" },
                                  { NULL, NULL } };

  int ret = conn->get_json_resource("/admin/log", pairs, log_info);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to fetch mdlog info" << dendl;
    return ret;
  }

  ldout(store->ctx(), 20) << "remote mdlog, num_shards=" << log_info.num_shards << dendl;

  RWLock::WLocker wl(ts_to_shard_lock);
  for (int i = 0; i < (int)log_info.num_shards; i++) {
    clone_markers.push_back(string());
    utime_shard ut;
    ut.shard_id = i;
    ts_to_shard[ut] = i;
  }

  ret = http_manager.set_threaded();
  if (ret < 0) {
    ldout(store->ctx(), 0) << "failed in http_manager.set_threaded() ret=" << ret << dendl;
    return ret;
  }

  return 0;
}

int RGWRemoteMetaLog::list_shards()
{
  for (int i = 0; i < (int)log_info.num_shards; i++) {
    int ret = list_shard(i);
    if (ret < 0) {
      ldout(store->ctx(), 10) << "failed to list shard: ret=" << ret << dendl;
    }
  }

  return 0;
}

int RGWRemoteMetaLog::list_shard(int shard_id)
{
  conn = store->rest_master_conn;

  char buf[32];
  snprintf(buf, sizeof(buf), "%d", shard_id);

  rgw_http_param_pair pairs[] = { { "type", "metadata" },
                                  { "id", buf },
                                  { NULL, NULL } };

  rgw_mdlog_shard_data data;
  int ret = conn->get_json_resource("/admin/log", pairs, data);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to fetch mdlog data" << dendl;
    return ret;
  }

  ldout(store->ctx(), 20) << "remote mdlog, shard_id=" << shard_id << " num of shard entries: " << data.entries.size() << dendl;

  vector<rgw_mdlog_entry>::iterator iter;
  for (iter = data.entries.begin(); iter != data.entries.end(); ++iter) {
    rgw_mdlog_entry& entry = *iter;
    ldout(store->ctx(), 20) << "entry: name=" << entry.name << dendl;
  }

  return 0;
}

int RGWRemoteMetaLog::get_shard_info(int shard_id)
{
  conn = store->rest_master_conn;

  char buf[32];
  snprintf(buf, sizeof(buf), "%d", shard_id);

  rgw_http_param_pair pairs[] = { { "type", "metadata" },
                                  { "id", buf },
                                  { "info", NULL },
                                  { NULL, NULL } };

  RGWMetadataLogInfo info;
  int ret = conn->get_json_resource("/admin/log", pairs, info);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to fetch mdlog info" << dendl;
    return ret;
  }

  ldout(store->ctx(), 20) << "remote mdlog, shard_id=" << shard_id << " marker=" << info.marker << dendl;

  return 0;
}

static void _aio_completion_notifier_cb(librados::completion_t cb, void *arg);

/* a single use librados aio completion notifier that hooks into the RGWCompletionManager */
class AioCompletionNotifier : public RefCountedObject {
  librados::AioCompletion *c;
  RGWCompletionManager *completion_mgr;
  void *user_data;

public:
  AioCompletionNotifier(RGWCompletionManager *_mgr, void *_user_data) : completion_mgr(_mgr), user_data(_user_data) {
    c = librados::Rados::aio_create_completion((void *)this, _aio_completion_notifier_cb, NULL);
  }

  ~AioCompletionNotifier() {
    c->release();
  }

  librados::AioCompletion *completion() {
    return c;
  }

  void cb() {
    completion_mgr->complete(user_data);
    put();
  }
};

static void _aio_completion_notifier_cb(librados::completion_t cb, void *arg)
{
  ((AioCompletionNotifier *)arg)->cb();
}

#define CLONE_MAX_ENTRIES 100
#define CLONE_OPS_WINDOW 16


struct RGWMetaSyncStatus {
  uint32_t num_shards;

  enum StateOptions {
    StateInit = 0,
    StateBuildingFullSyncMaps = 1,
    StateFullSync = 2,
    StateIncrementalSync = 3,
  };

  struct sync_marker {
    int state;
    string marker;

    sync_marker() : state((int)StateInit) {}
  };
  map<int, sync_marker> markers;

};

class RGWSyncStatusStore {
  RGWRados *store;
  librados::IoCtx ioctx;

  RGWMetaSyncStatus status;
  int read_status();
public:
  RGWSyncStatusStore(RGWRados *_store) : store(_store) {}

  int init();
};

#if 0
int RGWSyncStatusStore::read_status()
{
  string marker;

#define MAX_OMAP_ENTRIES 100
  do {
    map<string, bufferlist> vals;
    int r = ioctx.omap_get_vals(mdlog_sync_status_oid, marker, MAX_OMAP_ENTRIES, &vals);

    if (r < 0) {
      return r;
    }
    if (vals.size() != MAX_OMAP_ENTRIES) {
      break;
    }

    for (map<string, bufferlist>::iterator miter = vals.begin(); miter != vals.end(); ++miter) {
      const string& k = miter->first;
      bufferlist& bl = miter->second;

      bufferlist::iterator iter = bl.begin();
      try {
	::decode(s, iter);
	status.set_entry(shard_id, s);
      } catch (buffer::error& err) {
	ldout(store->ctx(), 0) << "ERROR: failed to decode entry for k=" << k << dendl;
      }
    }
  } while (true);

  return 0;
}
#endif

int RGWSyncStatusStore::init()
{
  const char *log_pool = store->get_zone_params().log_pool.name.c_str();
  librados::Rados *rados = store->get_rados_handle();
  int r = rados->ioctx_create(log_pool, ioctx);
  if (r < 0) {
    lderr(store->ctx()) << "ERROR: failed to open log pool (" << store->get_zone_params().log_pool.name << " ret=" << r << dendl;
    return r;
  }

#if 0
  r = read_status();
  if (r < 0) {
    return r;
  }
#endif

  return 0;
}

class RGWMetaSyncOp : public RGWAsyncOp {
  RGWRados *store;
  RGWMetadataLog *mdlog;
  RGWHTTPManager *http_manager;
  RGWSyncStatusStore sync_store;

  int shard_id;
  string marker;

  int max_entries;

  RGWRESTReadResource *http_op;

  enum State {
    Init                      = 0,
    ReadSyncStatus            = 1,
    ReadSyncStatusComplete    = 2,
    Done                      = 100,
    Error                     = 200,
  } state;

  int set_state(State s, int ret = 0) {
    state = s;
    return ret;
  }
public:
  RGWMetaSyncOp(RGWRados *_store, RGWHTTPManager *_mgr, RGWAsyncOpsManager *_ops_mgr,
		int _id) : RGWAsyncOp(_ops_mgr), store(_store),
                           mdlog(store->meta_mgr->get_log()),
                           http_manager(_mgr), sync_store(_store),
			   shard_id(_id),
                           max_entries(CLONE_MAX_ENTRIES),
			   http_op(NULL),
                           state(RGWMetaSyncOp::Init) {}

  int operate();

  int state_init();
  int state_read_sync_status();
  int state_read_sync_status_complete();

  bool is_done() { return (state == Done || state == Error); }
  bool is_error() { return (state == Error); }
};

int RGWMetaSyncOp::operate()
{
  switch (state) {
    case Init:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": init request" << dendl;
      return state_init();
    case ReadSyncStatus:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": reading shard status" << dendl;
      return state_read_sync_status();
    case ReadSyncStatusComplete:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": reading shard status complete" << dendl;
      return state_read_sync_status_complete();
    case Done:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": done" << dendl;
      break;
    case Error:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": error" << dendl;
      break;
  }

  return 0;
}

int RGWMetaSyncOp::state_init()
{
  int ret = sync_store.init();
  if (ret < 0) {
    return set_state(Error, ret);
  }
  return set_state(ReadSyncStatus);
}

int RGWMetaSyncOp::state_read_sync_status()
{
  return 0;
}

int RGWMetaSyncOp::state_read_sync_status_complete()
{
  return 0;
}

class RGWCloneMetaLogOp : public RGWAsyncOp {
  RGWRados *store;
  RGWMetadataLog *mdlog;
  RGWHTTPManager *http_manager;

  int shard_id;
  string marker;
  bool truncated;

  int max_entries;

  RGWRESTReadResource *http_op;

  AioCompletionNotifier *md_op_notifier;

  int req_ret;
  RGWMetadataLogInfo shard_info;
  rgw_mdlog_shard_data data;

  enum State {
    Init = 0,
    ReadShardStatus           = 1,
    ReadShardStatusComplete   = 2,
    SendRESTRequest           = 3,
    ReceiveRESTResponse       = 4,
    StoreMDLogEntries         = 5,
    StoreMDLogEntriesComplete = 6,
    Done                      = 100,
    Error                     = 200,
  } state;

  int set_state(State s, int ret = 0) {
    state = s;
    return ret;
  }
public:
  RGWCloneMetaLogOp(RGWRados *_store, RGWHTTPManager *_mgr, RGWAsyncOpsManager *_ops_mgr,
		    int _id, const string& _marker) : RGWAsyncOp(_ops_mgr), store(_store),
                                                      mdlog(store->meta_mgr->get_log()),
                                                      http_manager(_mgr), shard_id(_id),
                                                      marker(_marker), truncated(false), max_entries(CLONE_MAX_ENTRIES),
						      http_op(NULL), md_op_notifier(NULL),
						      req_ret(0),
                                                      state(RGWCloneMetaLogOp::Init) {}

  int operate();

  int state_init();
  int state_read_shard_status();
  int state_read_shard_status_complete();
  int state_send_rest_request();
  int state_receive_rest_response();
  int state_store_mdlog_entries();
  int state_store_mdlog_entries_complete();

  bool is_done() { return (state == Done || state == Error); }
  bool is_error() { return (state == Error); }
};

void RGWAsyncOpsManager::report_error(RGWAsyncOp *op)
{
#warning need to have error logging infrastructure that logs on backend
  lderr(cct) << "ERROR: failed operation: " << op->error_str() << dendl;
}

int RGWAsyncOpsManager::run(list<RGWAsyncOp *>& ops)
{
  int waiting_count = 0;
  for (list<RGWAsyncOp *>::iterator iter = ops.begin(); iter != ops.end(); ++iter) {
    RGWAsyncOp *op = *iter;
    int ret = op->operate();
    if (ret < 0) {
      ldout(cct, 0) << "ERROR: op->operate() returned ret=" << ret << dendl;
    }

    if (op->is_error()) {
      report_error(op);
    }

    if (op->is_blocked()) {
      waiting_count++;
    } else if (op->is_done()) {
      delete op;
    } else {
      ops.push_back(op);
    }

    if (waiting_count >= ops_window) {
      RGWCloneMetaLogOp *blocked_op;
      int ret = completion_mgr.get_next((void **)&blocked_op);
      if (ret < 0) {
	ldout(cct, 0) << "ERROR: failed to clone shard, completion_mgr.get_next() returned ret=" << ret << dendl;
      } else {
        waiting_count--;
      }
      blocked_op->set_blocked(false);
      if (!blocked_op->is_done()) {
	ops.push_back(blocked_op);
      } else {
	delete blocked_op;
      }
    }
  }

  while (waiting_count > 0) {
    RGWAsyncOp *op;
    int ret = completion_mgr.get_next((void **)&op);
    if (ret < 0) {
      ldout(cct, 0) << "ERROR: failed to clone shard, completion_mgr.get_next() returned ret=" << ret << dendl;
      return ret;
    } else {
      waiting_count--;
    }
  }

  return 0;
}

AioCompletionNotifier *RGWAsyncOpsManager::create_completion_notifier(RGWAsyncOp *op)
{
  return new AioCompletionNotifier(&completion_mgr, (void *)op);
}

int RGWRemoteMetaLog::clone_shards()
{
  list<RGWAsyncOp *> ops;
  for (int i = 0; i < (int)log_info.num_shards; i++) {
    RGWCloneMetaLogOp *op = new RGWCloneMetaLogOp(store, &http_manager, this, i, clone_markers[i]);
    ops.push_back(op);
  }

  return run(ops);
}

int RGWRemoteMetaLog::fetch()
{
  list<RGWAsyncOp *> ops;
  for (int i = 0; i < (int)log_info.num_shards; i++) {
    RGWCloneMetaLogOp *op = new RGWCloneMetaLogOp(store, &http_manager, this, i, clone_markers[i]);
    ops.push_back(op);
  }

  return run(ops);
}

int RGWCloneMetaLogOp::operate()
{
  switch (state) {
    case Init:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": init request" << dendl;
      return state_init();
    case ReadShardStatus:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": reading shard status" << dendl;
      return state_read_shard_status();
    case ReadShardStatusComplete:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": reading shard status complete" << dendl;
      return state_read_shard_status_complete();
    case SendRESTRequest:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": sending rest request" << dendl;
      return state_send_rest_request();
    case ReceiveRESTResponse:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": receiving rest response" << dendl;
      return state_receive_rest_response();
    case StoreMDLogEntries:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": storing mdlog entries" << dendl;
      return state_store_mdlog_entries();
    case StoreMDLogEntriesComplete:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": storing mdlog entries complete" << dendl;
      return state_store_mdlog_entries_complete();
    case Done:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": done" << dendl;
      break;
    case Error:
      ldout(store->ctx(), 20) << __func__ << ": shard_id=" << shard_id << ": error" << dendl;
      break;
  }

  return 0;
}

int RGWCloneMetaLogOp::state_init()
{
  data = rgw_mdlog_shard_data();

  return set_state(ReadShardStatus);
}

int RGWCloneMetaLogOp::state_read_shard_status()
{
  int ret = mdlog->get_info_async(shard_id, &shard_info, ops_mgr->get_completion_mgr(), (void *)this, &req_ret);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: mdlog->get_info_async() returned ret=" << ret << dendl;
    return set_state(Error, ret);
  }

  return yield(set_state(ReadShardStatusComplete));
}

int RGWCloneMetaLogOp::state_read_shard_status_complete()
{
  ldout(store->ctx(), 20) << "shard_id=" << shard_id << " marker=" << shard_info.marker << " last_update=" << shard_info.last_update << dendl;

  marker = shard_info.marker;

  return set_state(SendRESTRequest);
}

int RGWCloneMetaLogOp::state_send_rest_request()
{
  RGWRESTConn *conn = store->rest_master_conn;

  char buf[32];
  snprintf(buf, sizeof(buf), "%d", shard_id);

  char max_entries_buf[32];
  snprintf(max_entries_buf, sizeof(max_entries_buf), "%d", max_entries);

  const char *marker_key = (marker.empty() ? "" : "marker");

  rgw_http_param_pair pairs[] = { { "type", "metadata" },
                                  { "id", buf },
                                  { "max-entries", max_entries_buf },
                                  { marker_key, marker.c_str() },
                                  { NULL, NULL } };

  http_op = new RGWRESTReadResource(conn, "/admin/log", pairs, NULL, http_manager);

  http_op->set_user_info((void *)this);

  int ret = http_op->aio_read();
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to fetch mdlog data" << dendl;
    log_error() << "failed to send http operation: " << http_op->to_str() << " ret=" << ret << std::endl;
    http_op->put();
    return ret;
  }

  return yield(set_state(ReceiveRESTResponse));
}

int RGWCloneMetaLogOp::state_receive_rest_response()
{
  int ret = http_op->wait(&data);
  if (ret < 0) {
    error_stream << "http operation failed: " << http_op->to_str() << " status=" << http_op->get_http_status() << std::endl;
    ldout(store->ctx(), 0) << "ERROR: failed to wait for op, ret=" << ret << dendl;
    http_op->put();
    return set_state(Error, ret);
  }
  http_op->put();

  ldout(store->ctx(), 20) << "remote mdlog, shard_id=" << shard_id << " num of shard entries: " << data.entries.size() << dendl;

  truncated = ((int)data.entries.size() == max_entries);

  if (data.entries.empty()) {
    return set_state(Done);
  }

  return set_state(StoreMDLogEntries);
}


int RGWCloneMetaLogOp::state_store_mdlog_entries()
{
  list<cls_log_entry> dest_entries;

  vector<rgw_mdlog_entry>::iterator iter;
  for (iter = data.entries.begin(); iter != data.entries.end(); ++iter) {
    rgw_mdlog_entry& entry = *iter;
    ldout(store->ctx(), 20) << "entry: name=" << entry.name << dendl;

    cls_log_entry dest_entry;
    dest_entry.id = entry.id;
    dest_entry.section = entry.section;
    dest_entry.name = entry.name;
    dest_entry.timestamp = entry.timestamp;
  
    ::encode(entry.log_data, dest_entry.data);

    dest_entries.push_back(dest_entry);

    marker = entry.id;
  }

  AioCompletionNotifier *cn = ops_mgr->create_completion_notifier(this);

  int ret = store->meta_mgr->store_md_log_entries(dest_entries, shard_id, cn->completion());
  if (ret < 0) {
    cn->put();
    ldout(store->ctx(), 10) << "failed to store md log entries shard_id=" << shard_id << " ret=" << ret << dendl;
    return set_state(Error, ret);
  }
  return yield(set_state(StoreMDLogEntriesComplete));
}

int RGWCloneMetaLogOp::state_store_mdlog_entries_complete()
{
  if (truncated) {
    return state_init();
  }
  return set_state(Done);
}


int RGWMetadataSync::init()
{
  if (store->is_meta_master()) {
    return 0;
  }

  if (!store->rest_master_conn) {
    lderr(store->ctx()) << "no REST connection to master zone" << dendl;
    return -EIO;
  }

  int ret = master_log.init();
  if (ret < 0) {
    return ret;
  }

  return 0;
}



