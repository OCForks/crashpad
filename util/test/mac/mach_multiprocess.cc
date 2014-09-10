// Copyright 2014 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/test/mac/mach_multiprocess.h"

#include <AvailabilityMacros.h>
#include <bsm/libbsm.h>
#include <servers/bootstrap.h>

#include <string>

#include "base/auto_reset.h"
#include "base/logging.h"
#include "base/mac/scoped_mach_port.h"
#include "base/memory/scoped_ptr.h"
#include "base/rand_util.h"
#include "gtest/gtest.h"
#include "util/mach/bootstrap.h"
#include "util/mach/mach_extensions.h"
#include "util/misc/scoped_forbid_return.h"
#include "util/test/errors.h"
#include "util/test/mac/mach_errors.h"

namespace {

// The “hello” message contains a send right to the child process’ task port.
struct SendHelloMessage : public mach_msg_base_t {
  mach_msg_port_descriptor_t port_descriptor;
};

struct ReceiveHelloMessage : public SendHelloMessage {
  mach_msg_audit_trailer_t audit_trailer;
};

}  // namespace

namespace crashpad {
namespace test {

using namespace testing;

namespace internal {

struct MachMultiprocessInfo {
  MachMultiprocessInfo()
      : service_name(),
        local_port(MACH_PORT_NULL),
        remote_port(MACH_PORT_NULL),
        child_task(MACH_PORT_NULL) {}

  std::string service_name;
  base::mac::ScopedMachReceiveRight local_port;
  base::mac::ScopedMachSendRight remote_port;
  base::mac::ScopedMachSendRight child_task;  // valid only in parent
};

}  // namespace internal

MachMultiprocess::MachMultiprocess() : Multiprocess(), info_(NULL) {
}

void MachMultiprocess::Run() {
  ASSERT_EQ(NULL, info_);
  scoped_ptr<internal::MachMultiprocessInfo> info(
      new internal::MachMultiprocessInfo);
  base::AutoReset<internal::MachMultiprocessInfo*> reset_info(&info_,
                                                              info.get());

  return Multiprocess::Run();
}

MachMultiprocess::~MachMultiprocess() {
}

void MachMultiprocess::PreFork() {
  Multiprocess::PreFork();
  if (testing::Test::HasFatalFailure()) {
    return;
  }

  // Set up the parent port and register it with the bootstrap server before
  // forking, so that it’s guaranteed to be there when the child attempts to
  // look it up.
  info_->service_name = "com.googlecode.crashpad.test.mach_multiprocess.";
  for (int index = 0; index < 16; ++index) {
    info_->service_name.append(1, base::RandInt('A', 'Z'));
  }

  mach_port_t local_port;
  kern_return_t kr =
      BootstrapCheckIn(bootstrap_port, info_->service_name, &local_port);
  ASSERT_EQ(BOOTSTRAP_SUCCESS, kr)
      << BootstrapErrorMessage(kr, "bootstrap_check_in");
  info_->local_port.reset(local_port);
}

mach_port_t MachMultiprocess::LocalPort() const {
  EXPECT_NE(kMachPortNull, info_->local_port);
  return info_->local_port;
}

mach_port_t MachMultiprocess::RemotePort() const {
  EXPECT_NE(kMachPortNull, info_->remote_port);
  return info_->remote_port;
}

mach_port_t MachMultiprocess::ChildTask() const {
  EXPECT_NE(kMachPortNull, info_->child_task);
  return info_->child_task;
}

void MachMultiprocess::MultiprocessParent() {
  ReceiveHelloMessage message = {};

  kern_return_t kr =
      mach_msg(&message.header,
               MACH_RCV_MSG | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                   MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
               0,
               sizeof(message),
               info_->local_port,
               MACH_MSG_TIMEOUT_NONE,
               MACH_PORT_NULL);
  ASSERT_EQ(MACH_MSG_SUCCESS, kr) << MachErrorMessage(kr, "mach_msg");

  // Comb through the entire message, checking every field against its expected
  // value.
  EXPECT_EQ(MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, MACH_MSG_TYPE_MOVE_SEND) |
                MACH_MSGH_BITS_COMPLEX,
            message.header.msgh_bits);
  ASSERT_EQ(sizeof(SendHelloMessage), message.header.msgh_size);
  EXPECT_EQ(info_->local_port, message.header.msgh_local_port);
  ASSERT_EQ(1u, message.body.msgh_descriptor_count);
  EXPECT_EQ(static_cast<mach_msg_type_name_t>(MACH_MSG_TYPE_MOVE_SEND),
            message.port_descriptor.disposition);
  ASSERT_EQ(static_cast<mach_msg_descriptor_type_t>(MACH_MSG_PORT_DESCRIPTOR),
            message.port_descriptor.type);
  ASSERT_EQ(static_cast<mach_msg_trailer_type_t>(MACH_MSG_TRAILER_FORMAT_0),
            message.audit_trailer.msgh_trailer_type);
  ASSERT_EQ(sizeof(message.audit_trailer),
            message.audit_trailer.msgh_trailer_size);
  EXPECT_EQ(0u, message.audit_trailer.msgh_seqno);

  // Check the audit trailer’s values for sanity. This is a little bit of
  // overkill, but because the service was registered with the bootstrap server
  // and other processes will be able to look it up and send messages to it,
  // these checks disambiguate genuine failures later on in the test from those
  // that would occur if an errant process sends a message to this service.
#if MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_8
  uid_t audit_auid;
  uid_t audit_euid;
  gid_t audit_egid;
  uid_t audit_ruid;
  gid_t audit_rgid;
  pid_t audit_pid;
  au_asid_t audit_asid;
  audit_token_to_au32(message.audit_trailer.msgh_audit,
                      &audit_auid,
                      &audit_euid,
                      &audit_egid,
                      &audit_ruid,
                      &audit_rgid,
                      &audit_pid,
                      &audit_asid,
                      NULL);
#else
  uid_t audit_auid = audit_token_to_auid(message.audit_trailer.msgh_audit);
  uid_t audit_euid = audit_token_to_euid(message.audit_trailer.msgh_audit);
  gid_t audit_egid = audit_token_to_egid(message.audit_trailer.msgh_audit);
  uid_t audit_ruid = audit_token_to_ruid(message.audit_trailer.msgh_audit);
  gid_t audit_rgid = audit_token_to_rgid(message.audit_trailer.msgh_audit);
  pid_t audit_pid = audit_token_to_pid(message.audit_trailer.msgh_audit);
  au_asid_t audit_asid = audit_token_to_asid(message.audit_trailer.msgh_audit);
#endif
  EXPECT_EQ(geteuid(), audit_euid);
  EXPECT_EQ(getegid(), audit_egid);
  EXPECT_EQ(getuid(), audit_ruid);
  EXPECT_EQ(getgid(), audit_rgid);
  ASSERT_EQ(ChildPID(), audit_pid);

  auditinfo_addr_t audit_info;
  int rv = getaudit_addr(&audit_info, sizeof(audit_info));
  ASSERT_EQ(0, rv) << ErrnoMessage("getaudit_addr");
  EXPECT_EQ(audit_info.ai_auid, audit_auid);
  EXPECT_EQ(audit_info.ai_asid, audit_asid);

  // Retrieve the remote port from the message header, and the child’s task port
  // from the message body.
  info_->remote_port.reset(message.header.msgh_remote_port);
  info_->child_task.reset(message.port_descriptor.name);

  // Verify that the child’s task port is what it purports to be.
  int mach_pid;
  kr = pid_for_task(info_->child_task, &mach_pid);
  ASSERT_EQ(KERN_SUCCESS, kr) << MachErrorMessage(kr, "pid_for_task");
  ASSERT_EQ(ChildPID(), mach_pid);

  MachMultiprocessParent();

  info_->remote_port.reset();
  info_->local_port.reset();
}

void MachMultiprocess::MultiprocessChild() {
  ScopedForbidReturn forbid_return;;

  // local_port is not valid in the forked child process.
  ignore_result(info_->local_port.release());

  mach_port_t local_port;
  kern_return_t kr = mach_port_allocate(
      mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &local_port);
  ASSERT_EQ(KERN_SUCCESS, kr) << MachErrorMessage(kr, "mach_port_allocate");
  info_->local_port.reset(local_port);

  // The remote port can be obtained from the bootstrap server.
  mach_port_t remote_port;
  kr = bootstrap_look_up(
      bootstrap_port, info_->service_name.c_str(), &remote_port);
  ASSERT_EQ(BOOTSTRAP_SUCCESS, kr)
      << BootstrapErrorMessage(kr, "bootstrap_look_up");
  info_->remote_port.reset(remote_port);

  // The “hello” message will provide the parent with its remote port, a send
  // right to the child task’s local port receive right. It will also carry a
  // send right to the child task’s task port.
  SendHelloMessage message = {};
  message.header.msgh_bits =
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND) |
      MACH_MSGH_BITS_COMPLEX;
  message.header.msgh_size = sizeof(message);
  message.header.msgh_remote_port = info_->remote_port;
  message.header.msgh_local_port = info_->local_port;
  message.body.msgh_descriptor_count = 1;
  message.port_descriptor.name = mach_task_self();
  message.port_descriptor.disposition = MACH_MSG_TYPE_COPY_SEND;
  message.port_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

  kr = mach_msg(&message.header,
                MACH_SEND_MSG,
                message.header.msgh_size,
                0,
                MACH_PORT_NULL,
                MACH_MSG_TIMEOUT_NONE,
                MACH_PORT_NULL);
  ASSERT_EQ(MACH_MSG_SUCCESS, kr) << MachErrorMessage(kr, "mach_msg");

  MachMultiprocessChild();

  info_->remote_port.reset();
  info_->local_port.reset();

  if (Test::HasFailure()) {
    // Trigger the ScopedForbidReturn destructor.
    return;
  }

  forbid_return.Disarm();
}

}  // namespace test
}  // namespace crashpad
