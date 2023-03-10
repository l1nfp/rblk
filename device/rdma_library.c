#include "rdma_library.h"

static int
rblk_cma_event_handler(struct rdma_cm_id *id,
                       struct rdma_cm_event *event)
{
    int ret;
    rdma_ctx_t ctx = id->context;
    switch (event->event)
    {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        printk(KERN_DEBUG "rblk: RDMA_CM_EVENT_ADDR_RESOLVED.\n");
        ctx->state = ADDR_RESOLVED;
        ret = rdma_resolve_route(id, 2000);
        if (ret)
        {
            printk(KERN_ERR "rblk: rdma_resolve_route error %d\n", ret);
            wake_up_interruptible(&ctx->sem);
        }
        break;

    case RDMA_CM_EVENT_ROUTE_RESOLVED:
        printk(KERN_DEBUG "rblk: RDMA_CM_EVENT_ROUTE_RESOLVED.\n");
        ctx->state = ROUTE_RESOLVED;
        wake_up_interruptible(&ctx->sem);
        break;

    case RDMA_CM_EVENT_CONNECT_REQUEST:
        break;

    case RDMA_CM_EVENT_ESTABLISHED:
        ctx->state = CONNECTED;
        wake_up_interruptible(&ctx->sem);
        break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        printk(KERN_ERR "rblk: cma event %d, error %d\n", event->event,
               event->status);
        ctx->state = ERROR;
        wake_up_interruptible(&ctx->sem);
        break;

    case RDMA_CM_EVENT_DISCONNECTED:
        printk(KERN_ERR "rblk: DISCONNECT EVENT...\n");
        ctx->state = ERROR;
        wake_up_interruptible(&ctx->sem);
        break;

    case RDMA_CM_EVENT_DEVICE_REMOVAL:
        printk(KERN_ERR "rblk: cma detected device removal!!!!\n");
        ctx->state = ERROR;
        wake_up_interruptible(&ctx->sem);
        break;

    default:
        printk(KERN_ERR "oof bad type!\n");
        break;
    }
    return 0;
}

static void rblk_cq_event_handler(struct ib_cq *cq, void *userctx)
{
    int retval;
    struct ib_wc wc;
    rdma_ctx_t ctx;
    ctx = (rdma_ctx_t)userctx;
    // printk(KERN_DEBUG "step into rblk_cq_event_handler.\n");
    while ((retval = ib_poll_cq(ctx->rdma_cq, 1, &wc)) == 1)
    {
        if (wc.status != IB_WC_SUCCESS)
        {
            printk("Work completion (WC) has error status: %s",
                   ib_wc_status_msg(wc.status));
        }
        switch (wc.opcode)
        {
        case IB_WC_RECV:
            ctx->state = RDMA_RECV_COMPLETE;
            wake_up_interruptible(&ctx->sem);
            break;
        case IB_WC_RDMA_WRITE:
            ctx->state = RDMA_WRITE_COMPLETE;
            wake_up_interruptible(&ctx->sem);
            break;
        case IB_WC_RDMA_READ:
            ctx->state = RDMA_READ_COMPLETE;
            wake_up_interruptible(&ctx->sem);
            break;
        }
    }
    return;
}

rdma_ctx_t rdma_prepare_resources(int npages, char *ip_addr, int port)
{
    int retval;
    struct sockaddr_in sin4;
    rdma_ctx_t ctx;
    struct ib_cq_init_attr attr = {0};
    struct ib_qp_init_attr init_attr;
    ctx = kzalloc(sizeof(struct rdma_ctx), GFP_KERNEL);
    if (!ctx)
        return NULL;
    ctx->state = IDLE;
    init_waitqueue_head(&ctx->sem);
    // create id
    ctx->rdma_id = rdma_create_id(&init_net, rblk_cma_event_handler, ctx, RDMA_PS_TCP, IB_QPT_RC);
    if (!ctx->rdma_id)
    {
        printk(KERN_ERR "rdma_create_id error %d\n", retval);
        goto rdma_prepare_fail;
    }
    printk(KERN_DEBUG "rblk: created cm_id %p\n", ctx->rdma_id);
    // resolve addr, is succeed then this will trigger rdma_resolve_route
    sin4.sin_family = AF_INET;
    sin4.sin_port = htons(port);
    sin4.sin_addr.s_addr = in_aton(ip_addr);
    retval = rdma_resolve_addr(ctx->rdma_id, NULL, (struct sockaddr *)&sin4, RDMA_TIMEOUT);
    if (retval)
    {
        printk(KERN_ERR "Failed to resolve target address: \"%s\"\n", ip_addr);
        goto rdma_prepare_fail;
    }
    wait_event_interruptible(ctx->sem, ctx->state >= ROUTE_RESOLVED);
    if (ctx->state != ROUTE_RESOLVED)
    {
        printk(KERN_ERR "addr/route resolution did not resolve: state %d\n", ctx->state);
        goto rdma_prepare_fail;
    }
    // alloc pd
    ctx->rdma_pd = ib_alloc_pd(ctx->rdma_id->device, 0);
    if (!ctx->rdma_pd)
    {
        printk(KERN_ERR "ib_alloc_pd failed\n");
        goto rdma_prepare_fail;
    }
    printk(KERN_DEBUG "rblk: created pd %p\n", ctx->rdma_pd);
    // create cq
    attr.cqe = CQ_CAPACITY;
    attr.comp_vector = 0;
    ctx->rdma_cq = ib_create_cq(ctx->rdma_id->device, rblk_cq_event_handler, NULL,
                                ctx, &attr);
    if (!ctx->rdma_cq)
    {
        printk(KERN_ERR "ib_create_cq failed\n");
        goto rdma_prepare_fail;
    }
    printk(KERN_DEBUG "rblk: created cq %p\n", ctx->rdma_cq);
    retval = ib_req_notify_cq(ctx->rdma_cq, IB_CQ_NEXT_COMP);
    if (retval)
    {
        printk(KERN_ERR "Failed to request notifications\n");
        goto rdma_prepare_fail;
    }
    // finally, we can create qp
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = MAX_WR;
    init_attr.cap.max_recv_wr = MAX_WR;
    init_attr.cap.max_recv_sge = MAX_SGE;
    init_attr.cap.max_send_sge = MAX_SGE;
    init_attr.qp_type = IB_QPT_RC;
    init_attr.send_cq = ctx->rdma_cq;
    init_attr.recv_cq = ctx->rdma_cq;
    init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

    retval = rdma_create_qp(ctx->rdma_id, ctx->rdma_pd, &init_attr);
    if (retval)
    {
        printk(KERN_ERR "Failed to create qp\n");
        goto rdma_prepare_fail;
    }
    ctx->rdma_qp = ctx->rdma_id->qp;
    printk(KERN_DEBUG "rblk: created qp %p\n", ctx->rdma_qp);
    return ctx;
rdma_prepare_fail:
    rdma_clean_resources(ctx);
    return NULL;
}

// 8 bytes a line
void print_buffer(void *buffer, size_t len)
{

    int n;
    int i, idx;
    u8 *buf;
    if (buffer == NULL)
    {
        printk(KERN_DEBUG "pointer to rdma_meta_buf is NULL.\n");
        return;
    }
    n = len / 8;
    if (len % 8 != 0)
    {
        n += 1;
    }
    buf = kzalloc(sizeof(n * 8), GFP_KERNEL);
    memcpy(buf, buffer, len);
    printk(KERN_DEBUG "------------begin------------.\n");
    for (i = 0; i < n; i++)
    {
        idx = i * 8;
        printk(KERN_DEBUG "0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
               buf[idx], buf[idx + 1], buf[idx + 2], buf[idx + 3], buf[idx + 4], buf[idx + 5], buf[idx + 6], buf[idx + 7]);
    }
    printk(KERN_DEBUG "-------------end-------------.\n");
}

inline void show_rdma_buffer_attr(struct rdma_meta_attr *attr)
{
    if (!attr)
    {
        printk(KERN_DEBUG "pointer to rdma_meta_buf is NULL.\n");
        return;
    }
    printk(KERN_DEBUG "---------------------------------------------------------\n");
    printk(KERN_DEBUG "buffer attr, addr: %pK , size: %u , rkey : 0x%x \n",
           (void *)attr->address,
           (unsigned int)attr->length,
           attr->stag.remote_stag);
    printk(KERN_DEBUG "---------------------------------------------------------\n");
}

rdma_ctx_t rdma_init(int npages, char *ip_addr, int port, void *rdma_buffer, size_t buf_size)
{
    int retval;
    rdma_ctx_t ctx = NULL;
    struct rdma_conn_param conn_param;
    const struct ib_recv_wr *bad_wr;
    ctx = rdma_prepare_resources(npages, ip_addr, port);
    if (!ctx)
    {
        return NULL;
    }
    // try to connect remote rdma server
    memset(&conn_param, 0, sizeof conn_param);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 10;
    retval = rdma_connect(ctx->rdma_id, &conn_param);
    if (retval)
    {
        printk(KERN_ERR "rdma_connect error %d\n", retval);
        goto rdma_init_fail;
    }
    printk(KERN_DEBUG "rblk: waiting to connect to server.\n");
    wait_event_interruptible(ctx->sem, ctx->state >= CONNECTED);
    if (ctx->state != CONNECTED)
    {
        printk(KERN_ERR "wait for CONNECTED state %d\n", ctx->state);
        goto rdma_init_fail;
    }
    printk(KERN_DEBUG "rdma_connect successful \n");
    // issue request to receive metadata of remote buffer
    // (-_O) I suppose there is no function called ib_reg_mr to do similar things with ibv_reg_mr.
    ctx->recv_dma_addr = ib_dma_map_single(ctx->rdma_pd->device,
                                           &ctx->remote_meta_info,
                                           sizeof(ctx->remote_meta_info), DMA_BIDIRECTIONAL);
    ctx->recv_sge.addr = ctx->recv_dma_addr;
    ctx->recv_sge.length = sizeof(ctx->remote_meta_info);
    ctx->recv_sge.lkey = ctx->rdma_pd->local_dma_lkey;
    ctx->recv_wr.sg_list = &ctx->recv_sge;
    ctx->recv_wr.num_sge = 1;
    retval = ib_post_recv(ctx->rdma_qp, &ctx->recv_wr, &bad_wr);
    if (retval)
    {
        printk(KERN_ERR "ib_post_recv failed: %d\n", retval);
        goto rdma_init_fail;
    }
    wait_event_interruptible(ctx->sem,
                             ctx->state >= RDMA_RECV_COMPLETE);
    // we should have the metadata of remote buffer, let's see.
    show_rdma_buffer_attr(&ctx->remote_meta_info);
    ctx->rdma_dma_addr = ib_dma_map_single(ctx->rdma_pd->device,
                                           rdma_buffer,
                                           buf_size, DMA_BIDIRECTIONAL);
    ctx->dma_size = buf_size;
    return ctx;
rdma_init_fail:
    rdma_clean_resources(ctx);
    return NULL;
}

int rdma_exit(rdma_ctx_t ctx)
{
    int retval;
    if (ctx == NULL)
    {
        return 0;
    }
    /* active disconnect from the client side */
    if (ctx->rdma_id != NULL && ctx->state != IDLE)
    {
        retval = rdma_disconnect(ctx->rdma_id);
        if (retval)
        {
            printk(KERN_DEBUG "rdma_disconnect fail. \n");
            // continuing anyways
        }
    }
    rdma_clean_resources(ctx);
    return retval;
}

void rdma_clean_resources(rdma_ctx_t ctx)
{
    if (ctx == NULL)
    {
        return;
    }
    // unmap buffer
    if (ctx->recv_dma_addr != 0)
    {
        ib_dma_unmap_single(ctx->rdma_id->device, ctx->recv_dma_addr, sizeof(ctx->remote_meta_info), DMA_BIDIRECTIONAL);
    }
    if (ctx->rdma_dma_addr != 0)
    {
        ib_dma_unmap_single(ctx->rdma_id->device, ctx->rdma_dma_addr, ctx->dma_size, DMA_BIDIRECTIONAL);
    }
    // free cq then
    if (ctx->rdma_qp != NULL)
    {
        ib_destroy_qp(ctx->rdma_qp);
    }
    if (ctx->rdma_cq != NULL)
    {
        ib_destroy_cq(ctx->rdma_cq);
    }
    if (ctx->rdma_pd != NULL)
    {
        ib_dealloc_pd(ctx->rdma_pd);
    }
    // finally free rdma id
    if (ctx->rdma_id != NULL)
    {
        rdma_destroy_id(ctx->rdma_id);
    }
    kfree(ctx);
}

void do_rdma(rdma_ctx_t ctx, off_t offset, size_t len, int write)
{
    int retval;
    const struct ib_send_wr *bad_wr;
    enum rblk_rdma_state target_state;
    if (write)
    {
        printk(KERN_DEBUG "rblk: rmda write.\n");
    }
    else
    {
        printk(KERN_DEBUG "rblk: rmda read.\n");
    }

    ctx->rdma_sge.addr = ctx->rdma_dma_addr;
    ctx->rdma_sge.length = len;
    ctx->rdma_sge.lkey = ctx->rdma_pd->local_dma_lkey;

    ctx->rdma_wr.wr.sg_list = &ctx->rdma_sge;
    ctx->rdma_wr.wr.opcode = write == 1 ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ;
    ctx->rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
    ctx->rdma_wr.wr.num_sge = 1;
    ctx->rdma_wr.wr.next = NULL;
    ctx->rdma_wr.remote_addr = ctx->remote_meta_info.address + offset;
    ctx->rdma_wr.rkey = ctx->remote_meta_info.stag.remote_stag;
    ctx->state = WAITING_RDMA_OP;
    retval = ib_req_notify_cq(ctx->rdma_cq, IB_CQ_NEXT_COMP);
    if (retval)
    {
        printk(KERN_ERR "rblk: failed to request notifications\n");
        return;
    }
    retval = ib_post_send(ctx->rdma_qp, &ctx->rdma_wr.wr, &bad_wr);
    if (retval)
    {
        printk(KERN_ERR "rblk: ib_post_send fail.\n");
        return;
    }
    target_state = write == 1 ? RDMA_WRITE_COMPLETE : RDMA_READ_COMPLETE;
    wait_event_interruptible(ctx->sem, ctx->state >= target_state);
    if (ctx->state != target_state)
    {
        printk(KERN_ERR "wait for rblk_rdma_state %d state, but %d got.\n", target_state, ctx->state);
        return;
    }
    // printk(KERN_DEBUG "rblk: rdma r/w finish.\n");
}
