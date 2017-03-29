#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/nl80211.h>
#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>

static int
err_handler(struct sockaddr_nl *nl_addr,
            struct nlmsgerr    *err,
            void               *arg)
{
    int *ret = (int *)(arg);
    *ret = err->error;
    return NL_STOP;
}

static int
fin_handler(struct nl_msg *nl_msg,
            void          *arg)
{
    int *ret = (int *)(arg);
    *ret = 0;
    return NL_SKIP;
}

static int
ack_handler(struct nl_msg *nl_msg,
            void          *arg)
{
    int *ret = (int *)(arg);
    *ret = 0;
    return NL_STOP;
}

#define MINLEN sizeof(struct nl80211_sta_flag_update)
static struct nla_policy policy[NL80211_STA_INFO_MAX + 1] =
{
    [NL80211_STA_INFO_INACTIVE_TIME] = { .type = NLA_U32 },
    [NL80211_STA_INFO_STA_FLAGS]     = { .minlen = MINLEN }
};

static int
res_handler(struct nl_msg *nl_msg,
            void          *arg)
{
    struct nlattr     *attrs[NL80211_ATTR_MAX + 1];
    struct nlattr     *sta_attrs[NL80211_STA_INFO_MAX + 1];
    struct genlmsghdr *header        = NULL;
    uint8_t           *mac           = NULL;
    char               macaddr[20]   = {0};
    int32_t            inactive_ms   = -1;
    bool               authenticated = false;
    bool               authorized    = false;

    assert(nl_msg != NULL);

    header = nlmsg_data(nlmsg_hdr(nl_msg));
    nla_parse(attrs, NL80211_ATTR_MAX, genlmsg_attrdata(header, 0),
            genlmsg_attrlen(header, 0), NULL);
    if (!attrs[NL80211_ATTR_STA_INFO])
    {
        perror("Failed to parse station attributes");
        return NL_SKIP;
    }

    if (nla_parse_nested(sta_attrs, NL80211_STA_INFO_MAX,
                attrs[NL80211_ATTR_STA_INFO], policy))
    {
        perror("Failed to parse station nested attributes");
        return NL_SKIP;
    }

    mac = (uint8_t *)(nla_data(attrs[NL80211_ATTR_MAC]));
    snprintf(macaddr, sizeof(macaddr), "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (sta_attrs[NL80211_STA_INFO_INACTIVE_TIME])
        inactive_ms = nla_get_u32(sta_attrs[NL80211_STA_INFO_INACTIVE_TIME]);

    if (sta_attrs[NL80211_STA_INFO_STA_FLAGS])
    {
        struct nl80211_sta_flag_update *sta_flags = NULL;
        sta_flags = (struct nl80211_sta_flag_update *)
                nla_data(sta_attrs[NL80211_STA_INFO_STA_FLAGS]);
        #define BIT(x) (1ULL<<(x))
        authenticated = sta_flags->mask & BIT(NL80211_STA_FLAG_AUTHENTICATED) &&
            sta_flags->set & BIT(NL80211_STA_FLAG_AUTHENTICATED);
        authorized = sta_flags->mask & BIT(NL80211_STA_FLAG_AUTHORIZED) &&
            sta_flags->set & BIT(NL80211_STA_FLAG_AUTHORIZED);
    }

    printf("Station [%s] authenticated: [%u], authorized: [%u], inactive: [%d] ms\n",
            macaddr, authenticated, authorized, inactive_ms);

    return NL_SKIP;
}

#define WIFI_INTF_NAME "wlan0"
int main(int argc, const char *argv[])
{
    struct nl_sock *nl_sock    = NULL;
    struct nl_cb   *nl_sock_cb = NULL;
    struct nl_cb   *nl_cb      = NULL;
    struct nl_msg  *nl_msg     = NULL;
    int             nl_id      = -1;
    long int        devidx     = -1;
    int             err        = 0;
    int             ret        = EXIT_FAILURE;

    nl_sock = nl_socket_alloc();
    if (!nl_sock)
    {
        perror("Failed to create Netlink socket");
        return EXIT_FAILURE;
    }

    nl_socket_set_buffer_size(nl_sock, 8192, 8192);

    if (genl_connect(nl_sock) < 0)
    {
        perror("Failed to connect Netlink socket");
        goto EXIT;
    }

    nl_id = genl_ctrl_resolve(nl_sock, "nl80211");
    if (nl_id < 0)
    {
        perror("Failed to resolve NL80211");
        goto EXIT;
    }

    devidx = if_nametoindex(WIFI_INTF_NAME);
    if (devidx < 0)
    {
        perror("Failed to get device ID");
        goto EXIT;
    }

    nl_msg = nlmsg_alloc();
    if (!nl_msg)
    {
        perror("Failed to allocate Netlink message");
        goto EXIT;
    }

    nl_sock_cb = nl_cb_alloc(NL_CB_VERBOSE);
    nl_cb = nl_cb_alloc(NL_CB_VERBOSE);
    if (!nl_cb || !nl_sock_cb)
    {
        perror("Failed to allocate Netlink callbacks");
        goto EXIT;
    }

    genlmsg_put(nl_msg, 0, 0, nl_id, 0, NLM_F_DUMP, NL80211_CMD_GET_STATION, 0);
    NLA_PUT_U32(nl_msg, NL80211_ATTR_IFINDEX, devidx);

    nl_socket_set_cb(nl_sock, nl_sock_cb);

    if (nl_send_auto_complete(nl_sock, nl_msg) < 0)
    {
        perror("Failed to send Netlink message");
        goto EXIT;
    }

    err = 1;
    nl_cb_err(nl_cb, NL_CB_CUSTOM, err_handler, &err);
    nl_cb_set(nl_cb, NL_CB_FINISH, NL_CB_CUSTOM, fin_handler, &err);
    nl_cb_set(nl_cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);
    nl_cb_set(nl_cb, NL_CB_VALID, NL_CB_CUSTOM, res_handler, NULL);

    while (err > 0)
        nl_recvmsgs(nl_sock, nl_cb);

    if (err == 0)
        ret = EXIT_SUCCESS;

EXIT:
nla_put_failure:

    if (nl_cb)
        nl_cb_put(nl_cb);
    if (nl_sock_cb)
        nl_cb_put(nl_sock_cb);
    if (nl_msg)
        nlmsg_free(nl_msg);
    if (nl_sock)
        nl_socket_free(nl_sock);

    return ret;
}

