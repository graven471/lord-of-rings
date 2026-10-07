#include <stdint.h>
#define _GNU_SOURCE

#include <linux/io_uring.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * lords or ring fandom wiki:

core idea:
 rather than just communicate between kernel and user space with system calls,
 ring buffers are used as the main mode of communication.

 - setup shared buffer with `io_uring_setup(2)` and `mmap(2)`, mapping into user
space shared buffers for submission queue (SQ) and completion queue (CQ) You
place I/O requests you want to make on the SQ, while the kernel places the
results of those operations on the CQ.

 - For every I/O request you need to make (like to read a file, write a file,
accept a socket connection, etc), you create  a submission  queue  entry, or
SQE, describe the I/O operation you need to get done and add it to the tail of
the submission queue (SQ). Each I/O operation is, in essence, the equivalent of
a system call you would have made otherwise, if you were not using io_uring.

  - For instance, a SQE with the opcode set to IORING_OP_READ will request a
read operation to be  issued that is similar to the read(2) system call.

  - After you add one or more SQEs, you need to call io_uring_enter(2) to tell
the kernel to dequeue your I/O requests off the SQ and begin processing them.

  - For each SQE you submit, once it is done processing the request, the kernel
places a completion queue event or CQE at the tail of the completion queue or
CQ.  The kernel places exactly one matching CQE in the CQ for every SQE you
submit on  the SQ. After  you  retrieve a CQE, minimally, you might be
interested in checking the res field of the CQE structure, which corresponds to
the return value of the system call's equivalent, had you used it directly
without using  io_uring.

   - Given that  io_uring  is  an  async interface, errno is never used for
passing back error information. Instead, res will contain what the equivalent
system call would have returned in case of success, and in case of error res
will contain -errno.  For example, if the normal read system call would have
returned -1 and set errno to EINVAL, then res  would  contain  -EINVAL. If the
normal system call would have returned a read size of 1024, then res would
contain 1024.

   - It  is important to remember that I/O requests submitted to the kernel can
complete in any order. It is not necessary for the kernel to process one request
after another, in the order you placed them. Given that the interface is a ring,
the requests are attempted in order, however that doesn't imply  any  sort of
     ordering  on their execution or completion.  When more than one request is
in flight, it is not possible to determine which one will execute or complete
first.  When you dequeue CQEs off the CQ, you should always check which
submitted request it corresponds to. The most common method for doing so is
utilizing the user_data field in the request, which is passed back on the
completion side.

   - You add SQEs to the tail of the SQ.  The kernel reads SQEs off the head of
the queue.
   - The kernel adds CQEs to the tail of the CQ.  You read CQEs off the head of
the queue.

Submission queue polling
     One of the goals of io_uring is to provide a means for efficient I/O.  To
this end, io_uring supports a polling mode that lets you avoid the call  to
io_uring_enter(2),  which  you  use  to  inform the kernel that you have queued
SQEs on to the SQ.  With SQ Polling, io_uring starts a kernel thread that polls
the submission queue for any I/O requests you submit by adding SQEs.  With SQ
Polling enabled, there is no need for you to call io_uring_enter(2), letting you
avoid the  overhead of system calls. A designated kernel thread dequeues SQEs
off the SQ as you add them and dispatches them for asynchronous processing.


 - To submit an I/O request to io_uring, you need to acquire a submission queue
entry (SQE) from the submission queue (SQ), fill it up with details of  the
operation you  want  to submit and call io_uring_enter(2).  There are helper
functions of the form io_uring_prep_X to enable proper setup of the SQE. If you
want to avoid calling io_uring_enter(2), you have the option of setting up
Submission Queue Polling.
 */

int main(void) {
  printf("io_uring the lord of all rings\n");

  struct io_uring_params params = {
      // with this flag a kernel thread is created to perform submission queue
      // polling.
      // an io_uring instance configures in this way enables application to
      // issue I/O without
      // ever context switching into kernel y using the submission queue to fill
      // in new submission queue entries and watching for
      // completions on the completion queue, the application can submit and
      // reap I/Os without doing a single system call.
      // NOTE:  when using a ring setup with IORING_SETUP_SQPOLL, you never
      // directly call the io_uring_enter(2) system call.
      .flags = IORING_SETUP_SQPOLL,
  };

  // 32 is sq_entries
  int ring_fd = syscall(SYS_io_uring_setup, 32, &params);

  if (ring_fd < 0) {
    perror("io_uring_setup");
    exit(1);
  }

  // params is filled in by kernel, params.sq_off, params.cq_off,
  // params.features

  printf("lord_of_ring fd = %d\n", ring_fd);

  // Taken together, sq_entries and sq_off provide all of the information
  // necessary for accessing the submission queue ring buffer and the
  // submission queue entry array.

  // the addition of params.sq_off.array to the length of the region accounts
  // for the fact that the ring is located at the end of the data structure.
  void *sq_ring =
      mmap(0, params.sq_off.array + params.sq_entries * sizeof(uint32_t),
           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
           IORING_OFF_SQ_RING);

  void *cq_ring = mmap(
      0, params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe),
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
      IORING_OFF_CQ_RING);

  void *sq_entries = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          ring_fd, IORING_OFF_SQES);

  if (params.features & IORING_FEAT_SINGLE_MMAP) {
    // in this case we can combine SQ and CQ ring into one mmap
    // instead of two conceptually
    // [ mapping start ]
    // sq_off.head, sq_off.tail, sq_off.ring_mask ...  ← SQ control
    // ...
    // cq_off.head, cq_off.tail, cq_off.ring_mask ...  ← CQ control
    // cq_off.cqes ...                                  ← CQE structs live here
    // too [ mapping end ]
    printf("can combine sq_ and cq_ ring into one\n");
  }

  // The  head  and tail track the ring buffer state. The tail is incremented by
  // the application when submitting new I/O, and the head is incremented by the
  // kernel when the I/O has been successfully submitted.
  // we can access head ptr using head = (char*) sq_ring + params.sq_off.head

  if (sq_ring == NULL || cq_ring == NULL || sq_entries == NULL) {
    close(ring_fd);
    perror("mmap");
    exit(1);
  }

  close(ring_fd);
  // TODO: man page says: Closing the file descriptor returned by
  // io_uring_setup(2) will free all resources associated with the io_uring
  // context. Note that this  may  happen  asynchronously within the kernel, so
  // it is not guaranteed that resources are freed immediately.

  // munmap(sq_ring, params.sq_off.array + params.sq_entries *
  // sizeof(uint32_t)); munmap(cq_ring,
  //        params.cq_off.cqes + params.cq_entries * sizeof(struct
  //        io_uring_cqe));

  return 0;
}
