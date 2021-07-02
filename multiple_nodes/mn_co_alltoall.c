 /*
 * Author(s): Shigang Li <shigangli.cs@gmail.com>
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "immintrin.h"
#include "xmmintrin.h"
#include <math.h>
#include <malloc.h>

#define ITER 128
#define SIZES 20

int MN_CO_Alltoall(void* sendbuf, int sendcount, MPI_Datatype sendtype, void* recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm) {

  int32_t send_size;
  int id, nprocs;

  MPI_Type_size(sendtype, &send_size);
  MPI_Comm_rank(comm,&id);
  MPI_Comm_size(comm,&nprocs);

  void * tmp;
  tmp = memalign(4096, send_size*sendcount*nprocs);
  int groupsize = (int)sqrt((double)nprocs);
  int groupid = id / groupsize;
  int localid = id % groupsize;

  MPI_Request* send_reqs = NULL;
  MPI_Request* recv_reqs = NULL;
  send_reqs = (MPI_Request*)malloc(sizeof(MPI_Request) * groupsize);
  recv_reqs = (MPI_Request*)malloc(sizeof(MPI_Request) * groupsize);
  int i;
  int code;

  for(i = 0; i < groupsize; i++) {
    int m = (i+localid)%groupsize;
    int sourceid = groupid*groupsize+m;
    MPI_Irecv((void*)((uintptr_t)recvbuf + (recvcount * groupsize * send_size * m)), 
              groupsize*recvcount, recvtype, sourceid, 1024, comm, &recv_reqs[i]);
  }

  for(i = 0; i < groupsize; i++) {
      int m = (i+localid)%groupsize;
      int targetid = groupid*groupsize+m;
      MPI_Isend((void*)((uintptr_t)sendbuf + (sendcount * groupsize * send_size * m)), 
                groupsize*sendcount, sendtype, targetid, 1024, comm, &send_reqs[i]);
  }

  MPI_Waitall(groupsize, recv_reqs, MPI_STATUSES_IGNORE);
  MPI_Waitall(groupsize, send_reqs, MPI_STATUSES_IGNORE);


  //TODO: support non-power-of-2 groupsize
  for(code = 0; code < groupsize*groupsize; code++)
      memcpy((void*)((uintptr_t)tmp + _pext_u32(code, 0xAAAAAAAA)*sendcount*groupsize*send_size
             + _pext_u32(code, 0x55555555)*sendcount*send_size), 
             (void*)((uintptr_t)recvbuf + _pext_u32(code, 0x55555555)*sendcount*groupsize*send_size
             + _pext_u32(code, 0xAAAAAAAA)*sendcount*send_size), sendcount*send_size);


  for(i = 0; i < groupsize; i++) {
      int m = (i+localid)%groupsize;
      int sourceid = m*groupsize+groupid;
      MPI_Irecv((void*)((uintptr_t)recvbuf + (recvcount*groupsize*send_size*m)), 
                groupsize*recvcount, recvtype, sourceid, 2048, comm, &recv_reqs[i]);
  }

  for(i = 0; i < groupsize; i++) {
      int m = (i+groupid)%groupsize;
      int targetid = localid*groupsize+m;
      MPI_Isend((void*)((uintptr_t)tmp + (sendcount*groupsize*send_size*m)), 
                groupsize*sendcount, sendtype, targetid, 2048, comm, &send_reqs[i]);
  }

  MPI_Waitall(groupsize, recv_reqs, MPI_STATUSES_IGNORE);
  MPI_Waitall(groupsize, send_reqs, MPI_STATUSES_IGNORE);

  free(send_reqs);
  free(recv_reqs);
  free(tmp);

  return MPI_SUCCESS;
}



int main (int argc, char **argv)
{
    uint64_t datasize[SIZES] = {256,256,256,256,256,256,512,1024,2048,4096,8192,16384,
                                32768,65536,131072,262144,524288,1048576,2097152,4194304};

    uint64_t DATASIZE;
    int id, nprocs;
    double begin, elapse, felapse;
    MPI_Init(&argc,&argv);
    MPI_Comm_rank(MPI_COMM_WORLD,&id);
    MPI_Comm_size(MPI_COMM_WORLD,&nprocs);
    MPI_Comm node_comm;
    MPI_Comm net_comm;
    int node_rank;
    int node_size;
    int net_rank;
    int net_size;

    char proc_name[MPI_MAX_PROCESSOR_NAME];
    int proc_name_len;
    MPI_Get_processor_name(proc_name, &proc_name_len);
    int color = 0;
    char* s;
    for(s = proc_name; *s != '\0'; s++) {
        color = *s + 31 * color;
    }

    color &= 0x7FFFFFFF;
    MPI_Comm_split(MPI_COMM_WORLD, color, id, &node_comm);
    MPI_Comm_rank(node_comm, &node_rank);
    MPI_Comm_size(node_comm, &node_size);

    MPI_Comm_split(MPI_COMM_WORLD, node_rank, id, &net_comm);

    MPI_Comm_rank(net_comm, &net_rank);

    MPI_Comm_size(net_comm, &net_size);

    uint64_t *x, *y;
    int i,j,k;

    for(j=0; j<SIZES; j++) {
        MPI_Barrier(MPI_COMM_WORLD);
        DATASIZE = datasize[j];

        x=(uint64_t *)malloc(sizeof(uint64_t)*DATASIZE);
        y=(uint64_t *)malloc(sizeof(uint64_t)*DATASIZE);

        for(i=0; i<DATASIZE; i++) {
            x[i] = id;
        }

        for(i=0; i<DATASIZE; i++) {
            y[i] = 0;
        }

        MPI_Barrier(MPI_COMM_WORLD);
        elapse = 0;

        //net_size is a power-of-four
        if(net_size > 1 && node_rank==0) {
          //warmup
          MN_CO_Alltoall(x, DATASIZE/net_size, MPI_UNSIGNED_LONG_LONG, y, DATASIZE/net_size, MPI_UNSIGNED_LONG_LONG, net_comm);
          MN_CO_Alltoall(x, DATASIZE/net_size, MPI_UNSIGNED_LONG_LONG, y, DATASIZE/net_size, MPI_UNSIGNED_LONG_LONG, net_comm);
          begin = MPI_Wtime();

          for(i=0; i<ITER; i++) {
              MN_CO_Alltoall(x, DATASIZE/net_size, MPI_UNSIGNED_LONG_LONG, y, DATASIZE/net_size, MPI_UNSIGNED_LONG_LONG, net_comm);
          }
          elapse = MPI_Wtime() - begin;   

          MPI_Reduce(&elapse, &felapse, 1, MPI_DOUBLE, MPI_SUM, 0, net_comm);
            
          if(net_rank==0)
            printf("Alltoall, Datasize= %d, Process = %d, Total time = %f s\n", DATASIZE, id, felapse/ITER/net_size);
        }

        MPI_Barrier(MPI_COMM_WORLD);

        free(x);
        free(y);
    }
    MPI_Finalize();
    return 0;
}
