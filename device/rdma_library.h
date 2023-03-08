
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/inet.h>

/* Time out for resolve/route */
#define RDMA_TIMEOUT 2000
/* Capacity of the completion queue (CQ) */
#define CQ_CAPACITY (16)
/* MAX SGE capacity */
#define MAX_SGE (2)
/* MAX work requests */
#define MAX_WR (8)

#define RDMA_DEFAULT_PORT 20886
#define RDMA_DEFAULT_SERVER_ADDR "192.168.1.131"
/*Suppose page size is 4kB*/
#define RDMA_DEFAULT_REMOTE_NPAGE 1024 * 256

/*
 * These states are used to signal events between the completion handler
 * and the main thread.
 */
enum rblk_rdma_state
{
    IDLE = 1,
    ADDR_RESOLVED,
    ROUTE_RESOLVED,
    CONNECTED,
    RDMA_RECV_COMPLETE,
    WAITING_RDMA_OP,
    RDMA_WRITE_COMPLETE,
    RDMA_READ_COMPLETE,
    ERROR
};

/*
 * This is used to store metadata of remote buffer;
 */

struct __attribute((packed)) rdma_meta_attr
{
    uint64_t address;
    uint32_t length;
    union stag
    {
        /* if we send, we call it local stags */
        uint32_t local_stag;
        /* if we receive, we call it remote stag */
        uint32_t remote_stag;
    } stag;
};

/*
 * These are RDMA connection related resources.
 */
struct rdma_ctx
{
    unsigned long long int remote_mem_size;
    struct rdma_cm_id *rdma_id;
    struct ib_pd *rdma_pd;
    struct ib_cq *rdma_cq;
    struct ib_qp *rdma_qp;
    wait_queue_head_t sem;
    enum rblk_rdma_state state;
    // for recv metadata from server
    u64 recv_dma_addr;
    struct rdma_meta_attr remote_meta_info;
    struct ib_sge recv_sge;    /* recv single SGE */
    struct ib_recv_wr recv_wr; /* recv work request */

    // for rdma read/write operation, note that both write/read are rdma_wr;
    u64 rdma_dma_addr; /* record this for unregister */
    size_t dma_size;
    struct ib_sge rdma_sge;    /* single  read/write SGE */
    struct ib_rdma_wr rdma_wr; /* rdma send work request */
};

/*
 * rdma_ctx_t is a pointer to structre rdma_ctx, which contains contex about rmda;
 */
typedef struct rdma_ctx *rdma_ctx_t;

rdma_ctx_t rdma_init(int npages, char *ip_addr, int port, void *rdma_buffer, size_t buf_size);
int rdma_exit(rdma_ctx_t ctx);

rdma_ctx_t rdma_prepare_resources(int npages, char *ip_addr, int port);
void rdma_clean_resources(rdma_ctx_t ctx);

// if 'write'==1, write 'len' bytes data from 'buf' to remote buffer start at 'offset',
// or read the other way around.
void do_rdma(void *buf, size_t len, off_t offset, rdma_ctx_t ctx, int write);
