We create and open a shared memory object, and map the object to the virtual address space shared by the intra-node processes. Then, the shared address space is equally partitioned among the intra-node processes. The dynamic memory allocation functions are overridden so that each process can allocate and release memory on its own partition. 