#ifndef UTIL_RPC_H
#define UTIL_RPC_H

#include "util/common.h"
#include "util/coder.h"
#include <mpi.h>
#include <google/protobuf/message.h>

namespace dsm {

typedef google::protobuf::Message Message;

class RPCHelper {
public:
  RPCHelper(MPI::Comm *mpi) :
    mpi_world_(mpi), my_rank_(mpi->Get_rank()) {
  }

  // Try to read a message from the given peer and rpc channel; return false if no
  // message is immediately available.
  bool TryRead(int target, int method, Message *msg);
  bool HasData(int target, int method);
  bool HasData(int target, int method, MPI::Status &status);

  int Read(int target, int method, Message *msg);
  int ReadAny(int *target, int method, Message *msg);
  void Send(int target, int method, const Message &msg);
  void SyncSend(int target, int method, const Message &msg);

  MPI::Request SendData(int peer_id, int rpc_id, const string& data);

  // For whatever reason, MPI doesn't offer tagged broadcasts, we simulate that
  // here.
  void Broadcast(int method, const Message &msg);
  void SyncBroadcast(int method, const Message &msg);

private:
  boost::recursive_mutex mpi_lock_;

  MPI::Comm *mpi_world_;
  int my_rank_;
};
}

#endif // UTIL_RPC_H
