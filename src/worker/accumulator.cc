#include "util/common.h"
#include "worker/accumulator.h"

namespace upc {
LocalHash::LocalHash(ShardingFunction sf, HashFunction hf, AccumFunction af, int thread, int id) :
  SharedTable(sf, hf, af, thread, id) {
}

string LocalHash::get(const StringPiece &k) {
  return data[k.AsString()];
}

void LocalHash::put(const StringPiece &k, const StringPiece &v) {
  data[k.AsString()] = v.AsString();
}

void LocalHash::remove(const StringPiece &k) {
  data.erase(data.find(k.AsString()));
}

void LocalHash::clear() {
  data.clear();
}

bool LocalHash::contains(const StringPiece &k) {
  return data.find(k.AsString()) != data.end();
}

bool LocalHash::empty() {
  return data.empty();
}

int64_t LocalHash::size() {
  return data.size();
}

void LocalHash::applyUpdates(const HashUpdate& req) {
  for (int i = 0; i < req.put_size(); ++i) {
    const Pair &p = req.put(i);
    put(p.key(), p.value());
  }

  for (int i = 0; i < req.remove_size(); ++i) {
    const string& k = req.remove(i);
    remove(k);
  }
}

LocalHash::Iterator *LocalHash::get_iterator() {
  return new Iterator(this);
}

LocalHash::Iterator::Iterator(LocalHash *owner) : owner_(owner), it_(owner_->data.begin()) {}

string LocalHash::Iterator::key() {
  return it_->first;
}

string LocalHash::Iterator::value() {
  return it_->second;
}

void LocalHash::Iterator::next() {
  ++it_;
}

bool LocalHash::Iterator::done() {
  return it_ == owner_->data.end();
}

void PartitionedHash::ApplyUpdates(const upc::HashUpdate& req) {
  boost::recursive_mutex::scoped_lock sl(pending_lock_);
  partitions_[owner_thread_]->applyUpdates(req);
}

bool PartitionedHash::GetPendingUpdates(deque<LocalHash*> *out) {
  boost::recursive_mutex::scoped_lock sl(pending_lock_);

  for (int i = 0; i < partitions_.size(); ++i) {
    LocalHash *a = partitions_[i];
    if (i != owner_thread_ && !a->empty()) {
      partitions_[i] = new LocalHash(sf_, hf_, af_, i, table_id);
      out->push_back(a);
      while (accum_working_[i]);
    }
  }

  return out->size() > 0;
}

int PartitionedHash::pending_write_bytes() {
  boost::recursive_mutex::scoped_lock sl(pending_lock_);

  int64_t s = 0;
  for (int i = 0; i < partitions_.size(); ++i) {
    LocalHash *a = partitions_[i];
    if (i != owner_thread_) {
      s += a->size();
    }
  }

  return s;
}

void PartitionedHash::put(const StringPiece &k, const StringPiece &v) {
  int shard = sf_(k, partitions_.size());
  accum_working_[shard] = 1;
  LocalHash *h = partitions_[shard];
  h->put(k, v);
  accum_working_[shard] = 0;

  //LOG_EVERY_N(INFO, 100) << "Added key :: " << k.AsString() << " shard " << shard;
}

string PartitionedHash::get_local(const StringPiece &k) {
  int shard = sf_(k, partitions_.size());
  CHECK_EQ(shard, owner_thread_);

  accum_working_[shard] = 1;
  LocalHash *h = partitions_[shard];
  string v = h->get(k);
  accum_working_[shard] = 0;
  return v;
}

string PartitionedHash::get(const StringPiece &k) {
  int shard = sf_(k, partitions_.size());
  if (shard == owner_thread_) {
    return partitions_[shard]->get(k);
  }

  accum_working_[shard] = 1;
  if (partitions_[shard]->contains(k)) {
    string data = partitions_[shard]->get(k);
    accum_working_[shard] = 0;
    return data;
  }

  accum_working_[shard] = 0;
  VLOG(1) << "Requesting key " << k.AsString() << " from shard " << shard;
  HashRequest req;
  HashUpdate resp;
  req.set_key(k.AsString());
  req.set_table_id(table_id);

  rpc_->Send(shard, MTYPE_GET_REQUEST, req);
  rpc_->Read(shard, MTYPE_GET_RESPONSE, &resp);

  VLOG(1) << "Got key " << k.AsString() << " : " << resp.put(0).value();

  accum_working_[shard] = 1;
  partitions_[shard]->put(k, resp.put(0).value());
  accum_working_[shard] = 0;
  return resp.put(0).value();
}

void PartitionedHash::remove(const StringPiece &k) {
  LOG(FATAL) << "Not implemented!";
}

}
