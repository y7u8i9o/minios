#pragma once
#include <net/tcp.h>
#include <net/wire.h>
#include <net/ipv4.h>
#include <net/worker.h>
#include <net/clock.h>
#include <ipc/socket.h>
#include <sched/wait.h>

#define TCP_ENDPOINTS 64
#define TCP_CONNECTIONS 64
#define TCP_SYN_BACKLOG 8
#define TCP_ACCEPT_BACKLOG 16
/* Receive and send storage is allocated per connection (N13). The window
 * shift is the smallest one that expresses the whole receive store in the
 * 16-bit window field. */
#define TCP_RECEIVE_CAPACITY 131072
#define TCP_WINDOW_SHIFT 2
#define TCP_LOCAL_MSS 1460
#define TCP_DEFAULT_MSS 536
#define TCP_RETRY_MS 1000
#define TCP_MAX_RETRIES 3
#define TCP_SEND_CAPACITY 65536
/* RFC 7323 sets the largest shift a peer may use, the timestamp option
 * takes 12 bytes with its two padding bytes, and PAWS no longer trusts
 * TS.Recent after 24 idle days. */
#define TCP_MAX_WINDOW_SHIFT 14
#define TCP_TIMESTAMP_SPACE 12
#define TCP_PAWS_IDLE_MS (24ull * 24 * 60 * 60 * 1000)
/* Following RFC 2018 and RFC 6675 (N14), at most four blocks are reported
 * to the peer, the scoreboard retains at most eight disjoint SACKed ranges,
 * and three duplicate ACKs or SACKed segments declare a loss. */
#define TCP_SACK_REPORT 4
#define TCP_SCOREBOARD 8
#define TCP_DUP_THRESHOLD 3
/* RFC 1122 section 4.2.3.2 and RFC 9293 section 3.8.6.3 allow an ACK to be
 * delayed at most 500 ms. This stack delays it 100 ms and acknowledges at
 * once when two full segments of data are unacknowledged. */
#define TCP_DELAYED_ACK_MS 100
_Static_assert((TCP_RECEIVE_CAPACITY >> TCP_WINDOW_SHIFT) <= 65535 &&
                   (TCP_RECEIVE_CAPACITY >> (TCP_WINDOW_SHIFT - 1)) > 65535,
               "TCP_WINDOW_SHIFT must be the smallest shift for the receive store");
#define TCP_DATA_RETRIES 8
#define TCP_PROGRESS_MS 120000
#define TCP_ORPHAN_MS 30000
#define TCP_TIME_WAIT_MS 120000

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_connection;
/* All endpoint fields are protected by tcp_lock. File references provide
 * endpoint lifetime; no timer or packet queue retains an endpoint reference. */
struct tcp_endpoint {
    struct socket *socket;
    struct tcp_connection *connection;
    struct waitq wait;
    uint32_t local_address;
    uint32_t peer_address;
    uint16_t local_port;
    uint16_t peer_port;
    unsigned backlog;
    unsigned pending;
    bool listening;
    bool connecting;
    bool connected;
    bool writable;
    bool eof;
    bool write_closed;
    int terminal_error;
};

/* Fields belong to netd except receive_head/count/data, which tcp_lock
 * protects. Attached connections cannot be freed until endpoint close runs
 * on netd. Unattached children belong to their listener until accepted or
 * expired; detached closing connections belong solely to their timer. */
struct tcp_connection {
    bool used;
    enum tcp_state state;
    struct tcp_endpoint *endpoint;
    struct tcp_endpoint *listener;
    bool accept_ready;
    uint64_t accept_order;
    uint32_t local_address, peer_address;
    uint16_t local_port, peer_port;
    uint32_t iss, irs;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint32_t snd_wl1, snd_wl2;
    /* peer_window is in bytes, already shifted by snd_scale. */
    uint32_t peer_window;
    uint16_t peer_mss, local_mss;

    /* These fields implement RFC 7323. rcv_scale is the shift offered in
     * our SYN and applies to the windows we advertise once window_scaling
     * is negotiated; snd_scale is the peer's shift, applied to every window
     * it sends outside SYN. ts_recent is the peer timestamp echoed in our
     * segments, ts_recent_age the network clock when it was last updated,
     * last_ack_sent the acknowledgement of our latest segment, ts_offset
     * the random offset of our timestamp clock and ts_sample_end the
     * sequence an ACK must cover before the next timestamp RTT sample (one
     * sample per flight). */
    bool window_scaling, timestamps, ts_sample_valid;
    uint8_t snd_scale, rcv_scale;
    uint32_t ts_recent, ts_offset, last_ack_sent, ts_sample_end;
    uint64_t ts_recent_age;

    /* These fields implement RFC 2018 and RFC 6675 (N14). sack_report
     * lists the out-of-order ranges we report, most recent first. The
     * scoreboard lists the ranges the peer reported, sorted, disjoint and
     * within [snd_una, snd_nxt]. high_rxt is the end of the highest
     * retransmission in the current recovery; rto_recovery marks a recovery
     * started by a timeout, in which every unSACKed byte below recovery_end
     * counts as lost. */
    bool sack, rto_recovery;
    struct tcp_range {
        uint32_t start, end;
    } sack_report[TCP_SACK_REPORT], scoreboard[TCP_SCOREBOARD];
    unsigned sack_report_count, scoreboard_count;
    uint32_t high_rxt;

    /* For delayed acknowledgements, ack_deadline is armed when an ACK is
     * owed, ack_owed counts the bytes received since the last ACK we sent,
     * and rcv_adv is the right edge of the window that ACK advertised. */
    uint64_t ack_deadline;
    uint32_t ack_owed, rcv_adv;
    bool peer_fin, read_shutdown;
    bool fin_requested, fin_sent;
    uint32_t fin_sequence;
    int error;

    /* The prefix transmit_sent is on the wire. All transmit_length bytes
     * remain owned here until cumulative acknowledgement or terminal error.
     * transmit, receive and receive_present are allocated with the
     * connection and released with it, or earlier by tcp_release_unused
     * once no endpoint can use them (the pointers are then NULL). */
    uint8_t *transmit;
    size_t transmit_length;
    size_t transmit_sent;
    uint32_t congestion_window, slow_start_threshold, congestion_credit;
    uint32_t recovery_end;
    unsigned duplicate_acks, data_retries;
    bool recovering;
    uint32_t srtt_ms, rtt_variance_ms, rto_ms;
    uint64_t sample_time, progress_deadline, last_transmit;
    uint32_t sample_end;
    bool sampling;

    /* Present bits describe out-of-order bytes in the same circular storage
     * as readable data. Only contiguous bytes contribute to receive_count. */
    uint8_t *receive;
    uint8_t *receive_present;
    unsigned receive_head, receive_count, out_of_order;
    bool pending_fin;
    uint32_t pending_fin_sequence;

    struct net_timer timer;
    uint64_t control_deadline;
    uint64_t data_deadline;
    uint64_t lifetime_deadline;
    unsigned retries;
};


extern struct spinlock tcp_lock;
extern struct tcp_endpoint tcp_endpoints[TCP_ENDPOINTS];
extern struct tcp_connection tcp_connections[TCP_CONNECTIONS];
extern struct tcp_stats tcp_counters;
extern const struct socket_ops tcp_socket_ops;

static inline bool tcp_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}
static inline bool tcp_after(uint32_t a, uint32_t b)
{
    return tcp_before(b, a);
}
/* tcp_segment_window returns the peer's window of a segment other than a
 * SYN in bytes. */
static inline uint32_t tcp_segment_window(const struct tcp_connection *c,
                                          const struct tcp_segment *segment)
{
    return (uint32_t)segment->window << c->snd_scale;
}

int tcp_endpoint_create(struct socket *socket);
int tcp_bind_port(struct tcp_endpoint *endpoint, uint32_t address, uint16_t port);
struct tcp_connection *tcp_connection_alloc(void);
void tcp_connection_free(struct tcp_connection *connection);
void tcp_fail(struct tcp_connection *connection, int error);
void tcp_publish(struct tcp_connection *connection);
void tcp_publish_listener(struct tcp_endpoint *listener);
void tcp_listener_counts(struct tcp_endpoint *listener, unsigned *incomplete, unsigned *ready);
uint32_t tcp_initial_sequence(void);
void tcp_established(struct tcp_connection *connection);
void tcp_enter_time_wait(struct tcp_connection *connection);
void tcp_maybe_fin(struct tcp_connection *connection);
unsigned tcp_receive_window(struct tcp_connection *connection);
unsigned tcp_receive_window_locked(struct tcp_connection *connection);
void tcp_release_storage(struct tcp_connection *connection);
void tcp_release_receive(struct tcp_connection *connection);
void tcp_release_transmit(struct tcp_connection *connection);
void tcp_release_unused(struct tcp_connection *connection);
uint32_t tcp_timestamp_now(const struct tcp_connection *connection);
unsigned tcp_send_mss(const struct tcp_connection *connection);
void tcp_negotiate(struct tcp_connection *connection, const struct tcp_segment *syn);
unsigned tcp_receive_mss(const struct tcp_connection *connection);
void tcp_acknowledge(struct tcp_connection *connection, bool immediate);
void tcp_window_update(struct tcp_connection *connection);
int tcp_emit(struct tcp_connection *connection,
             uint8_t flags,
             uint32_t sequence,
             const void *data,
             size_t length);
void tcp_reset_reply(const struct tcp_segment *segment);
void tcp_challenge_ack(struct tcp_connection *connection);
void tcp_schedule(struct tcp_connection *connection);
void tcp_timer_expire(struct net_timer *timer);
void tcp_start_control_timer(struct tcp_connection *connection);
uint16_t tcp_checksum(uint32_t source, uint32_t destination, const void *data, size_t length);

void tcp_transfer_init(struct tcp_connection *connection);
void tcp_flush(struct tcp_connection *connection);
void tcp_data_ack(struct tcp_connection *connection, const struct tcp_segment *segment);
void tcp_data_timeout(struct tcp_connection *connection);
void tcp_receive_segment(struct tcp_connection *connection, const struct tcp_segment *segment);
void tcp_receive_discard(struct tcp_connection *connection);

bool tcp_random_ready(void);
