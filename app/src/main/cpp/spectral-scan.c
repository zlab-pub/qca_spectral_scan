#include <android/log.h>
#include <errno.h>
#include <inttypes.h>
#include <jni.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netlink/attr.h>
#include <netlink/errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <pthread.h>
#include <qca-vendor.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG_TAG "spectral-scan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static struct {
  atomic_bool running;
  int *ap_freqs;
  int ap_freqs_count;
  uint32_t fft_size;
  atomic_uint_least32_t ap_freq;
  atomic_uint_least32_t scan_freq;
  struct sockaddr_un saddr_forward;
  int sock_forward;
  unsigned ifindex;
  atomic_uint_least32_t ap_ifindex;
  int send_fam;
  struct nl_sock *nl_sock_send;
  struct nl_sock *nl_sock_recv;
  struct nl_sock *nl_sock_ap_ctrl;
  struct nl_sock *nl_sock_ap_event;
  pthread_t ap_ctrl_thread;
  pthread_t scan_thread;
  pthread_t forward_thread;
} state;

static void handle_sigint(int sig) {}

static void switch_ap_freq(int freq) {
  if (state.ap_ifindex == 0) {
    LOGE("Can't get AP interface index: %s", strerror(errno));
    return;
  }

  struct nl_msg *msg = nlmsg_alloc();
  if (msg == NULL) {
    LOGE("Can't allocate Netlink message for AP channel switch");
    return;
  }

  if (genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, state.send_fam, 0,
                  NLM_F_REQUEST, NL80211_CMD_CHANNEL_SWITCH, 0) == NULL) {
    LOGE("Can't add Generic Netlink header for AP channel switch");
    goto nla_put_failure;
  }

  NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, state.ap_ifindex);
  NLA_PUT_U32(msg, NL80211_ATTR_CH_SWITCH_COUNT, 1);
  NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_FREQ, (uint32_t)freq);
  NLA_PUT(msg, NL80211_ATTR_BEACON_TAIL, 0, NULL);

  struct nlattr *nest = nla_nest_start(msg, NL80211_ATTR_CSA_IES);
  if (nest == NULL) {
    goto nla_put_failure;
  }

  const uint8_t tail[] = {37, 3, 0, 0, 1};
  NLA_PUT(msg, NL80211_ATTR_BEACON_TAIL, sizeof(tail), tail);
  NLA_PUT_U16(msg, NL80211_ATTR_CSA_C_OFF_BEACON, sizeof(tail) - 1);

  if (nla_nest_end(msg, nest) < 0) {
    goto nla_put_failure;
  }

  int nl_err = nl_send_sync(state.nl_sock_ap_ctrl, msg);
  if (nl_err < 0 && (nl_err != -NLE_INVAL || (int)state.ap_freq != freq)) {
    LOGE("Can't switch AP channel to %d MHz: %s", freq, nl_geterror(nl_err));
  }

  return;
nla_put_failure:
  nlmsg_free(msg);
}

static void *ap_ctrl_thread(void *arg) {
  struct sigaction sa = {.sa_handler = handle_sigint};
  sigaction(SIGINT, &sa, NULL);

  if (state.ap_freqs_count <= 0) {
    return NULL;
  }

  int chan_idx = 0;
  unsigned counter = 0;
  while (state.running) {
    if (counter == 0) {
      switch_ap_freq(state.ap_freqs[chan_idx]);
    }
    usleep(20000);
    if (++counter >= 50) {
      counter = 0;
      chan_idx++;
      chan_idx %= state.ap_freqs_count;
    }
  }

  return NULL;
}

static void check_ap_freq() {
  const int sock = nl_socket_get_fd(state.nl_sock_ap_event);

  for (;;) {
    uint8_t msg[4096];
    const ssize_t msg_len = recv(sock, msg, sizeof(msg), MSG_DONTWAIT);
    if (msg_len < 0) {
      return;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)msg;
    if (!nlmsg_ok(nlh, (int)msg_len)) {
      continue;
    }
    if (!genlmsg_valid_hdr(nlh, 0)) {
      continue;
    }

    const struct genlmsghdr *gnlh = genlmsg_hdr(nlh);
    if (gnlh->cmd != NL80211_CMD_CH_SWITCH_NOTIFY) {
      continue;
    }

    const struct nlattr *nla;
    int rem;
    nla_for_each_attr(nla, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0),
                      rem) {
      if (nla_type(nla) == NL80211_ATTR_IFINDEX) {
        state.ap_ifindex = nla_get_u32(nla);
      } else if (nla_type(nla) == NL80211_ATTR_WIPHY_FREQ) {
        state.ap_freq = nla_get_u32(nla);
      }
    }
  }
}

static void *scan_thread(void *arg) {
  struct sigaction sa = {.sa_handler = handle_sigint};
  sigaction(SIGINT, &sa, NULL);

  while (state.running) {
    struct nl_msg *msg_start = nlmsg_alloc();
    if (msg_start == NULL) {
      LOGE("Can't allocate Netlink message for scan start");
      continue;
    }

    struct nl_msg *msg_stop = nlmsg_alloc();
    if (msg_stop == NULL) {
      LOGE("Can't allocate Netlink message for scan stop");
      nlmsg_free(msg_start);
      continue;
    }

    if (genlmsg_put(msg_start, NL_AUTO_PORT, NL_AUTO_SEQ, state.send_fam, 0, 0,
                    NL80211_CMD_VENDOR, 0) == NULL) {
      LOGE("Can't add Generic Netlink header for scan start");
      goto nla_put_failure;
    }
    if (genlmsg_put(msg_stop, NL_AUTO_PORT, NL_AUTO_SEQ, state.send_fam, 0, 0,
                    NL80211_CMD_VENDOR, 0) == NULL) {
      LOGE("Can't add Generic Netlink header for scan stop");
      goto nla_put_failure;
    }

    NLA_PUT_U32(msg_start, NL80211_ATTR_IFINDEX, state.ifindex);
    NLA_PUT_U32(msg_stop, NL80211_ATTR_IFINDEX, state.ifindex);

    NLA_PUT_U32(msg_start, NL80211_ATTR_VENDOR_ID, OUI_QCA);
    NLA_PUT_U32(msg_stop, NL80211_ATTR_VENDOR_ID, OUI_QCA);

    NLA_PUT_U32(msg_start, NL80211_ATTR_VENDOR_SUBCMD,
                QCA_NL80211_VENDOR_SUBCMD_SPECTRAL_SCAN_START);
    NLA_PUT_U32(msg_stop, NL80211_ATTR_VENDOR_SUBCMD,
                QCA_NL80211_VENDOR_SUBCMD_SPECTRAL_SCAN_STOP);

    struct nlattr *nest = nla_nest_start(msg_start, NL80211_ATTR_VENDOR_DATA);
    if (nest == NULL) {
      LOGE("Can't start config data");
      goto nla_put_failure;
    }

#define SPECTRAL_CONFIG(k, v)                                                  \
  NLA_PUT_U32(msg_start, QCA_WLAN_VENDOR_ATTR_SPECTRAL_SCAN_CONFIG_##k, (v))
    SPECTRAL_CONFIG(SCAN_COUNT, 0);
    SPECTRAL_CONFIG(SCAN_PERIOD, 0);
    SPECTRAL_CONFIG(FFT_SIZE, state.fft_size);
    SPECTRAL_CONFIG(INIT_DELAY, 0);
    SPECTRAL_CONFIG(PWR_FORMAT, 1);
    SPECTRAL_CONFIG(RPT_MODE, 3);
    SPECTRAL_CONFIG(DBM_ADJ, 1);
#undef SPECTRAL_CONFIG

    int nl_err = nla_nest_end(msg_start, nest);
    if (nl_err < 0) {
      LOGE("Can't end config data: %s", nl_geterror(nl_err));
      goto nla_put_failure;
    }

    check_ap_freq();

    nl_err = nl_send_sync(state.nl_sock_send, msg_start);
    if (nl_err < 0) {
      LOGE("Can't start spectral scan: %s", nl_geterror(nl_err));
    }

    state.scan_freq = state.ap_freq;
    usleep(10000);

    nl_err = nl_send_sync(state.nl_sock_send, msg_stop);
    if (nl_err < 0) {
      LOGE("Can't stop spectral scan: %s", nl_geterror(nl_err));
    }

    continue;
  nla_put_failure:
    nlmsg_free(msg_start);
    nlmsg_free(msg_stop);
  }

  return NULL;
}

static void *forward_thread(void *arg) {
  struct sigaction sa = {.sa_handler = handle_sigint};
  sigaction(SIGINT, &sa, NULL);

  const int sock_recv = nl_socket_get_fd(state.nl_sock_recv);

  while (state.running) {
    uint8_t msg[4096];
    const ssize_t msg_len = recv(sock_recv, msg, sizeof(msg), 0);
    if (msg_len < 0) {
      continue;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)msg;
    if (!nlmsg_ok(nlh, (int)msg_len)) {
      continue;
    }
    if (!genlmsg_valid_hdr(nlh, 0)) {
      continue;
    }

    enum { WLAN_NL_MSG_SPECTRAL_SCAN = 29 };

    const struct genlmsghdr *gnlh = genlmsg_hdr(nlh);
    if (gnlh->cmd != WLAN_NL_MSG_SPECTRAL_SCAN) {
      continue;
    }

    enum cld80211_attr {
      CLD80211_ATTR_VENDOR_DATA = 1,
      CLD80211_ATTR_DATA,
      CLD80211_ATTR_META_DATA,
      CLD80211_ATTR_CMD,
      CLD80211_ATTR_CMD_TAG_DATA,
    };

    const struct nlattr *nest_nla = genlmsg_attrdata(gnlh, 0);
    if (!nla_ok(nest_nla, genlmsg_attrlen(gnlh, 0))) {
      continue;
    }
    if (nla_type(nest_nla) != CLD80211_ATTR_VENDOR_DATA) {
      continue;
    }

    const struct nlattr *nla = nla_data(nest_nla);
    if (!nla_ok(nla, nla_len(nest_nla))) {
      continue;
    }
    if (nla_type(nla) != CLD80211_ATTR_DATA) {
      continue;
    }

    const uint8_t *samp_buf = nla_data(nla);
    const int samp_len = nla_len(nla);

    if (samp_len < 93) {
      continue;
    }

    if (*(uint16_t *)(samp_buf + 4) == 0) {
      *(uint16_t *)(samp_buf + 4) = (uint16_t)state.scan_freq;
    }

    if (sendto(state.sock_forward, samp_buf, (size_t)samp_len, 0,
               (struct sockaddr *)&state.saddr_forward,
               sizeof(state.saddr_forward)) < 0) {
      LOGW("Can't forward data: %s", strerror(errno));
      continue;
    }
  }

  return NULL;
}

JNIEXPORT void JNICALL Java_com_example_spectral_1plot_ScanService_startScan(
    JNIEnv *env, jobject obj, jintArray apFreqs, jint fftSize,
    jstring sockPath) {
  if (state.running) {
    return;
  }

  const char *sock_path = (*env)->GetStringUTFChars(env, sockPath, NULL);
  if (sock_path == NULL) {
    LOGE("Can't get forward socket path");
    return;
  }

  memset(&state.saddr_forward, 0, sizeof(state.saddr_forward));
  state.saddr_forward.sun_family = AF_UNIX;
  const size_t sun_path_len =
      sizeof(struct sockaddr_un) - offsetof(struct sockaddr_un, sun_path);
  strlcpy(state.saddr_forward.sun_path, sock_path, sun_path_len);

  (*env)->ReleaseStringUTFChars(env, sockPath, sock_path);
  sock_path = NULL;

  int sock_forward = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sock_forward < 0) {
    LOGE("Can't create forward socket: %s", strerror(errno));
    return;
  }

  char ifname[PROP_VALUE_MAX] = "";
  const prop_info *pi = __system_property_find("wifi.interface");
  if (pi != NULL) {
    __system_property_read(pi, NULL, ifname);
  }
  if (ifname[0] == '\0') {
    strlcpy(ifname, "wlan0", sizeof(ifname));
  }
  unsigned ifindex = if_nametoindex(ifname);
  if (ifindex == 0) {
    LOGE("Can't get WLAN interface index: %s", strerror(errno));
    return;
  }

  char ap_ifname[PROP_VALUE_MAX] = "";
  pi = __system_property_find("ro.vendor.wifi.sap.interface");
  if (pi != NULL) {
    __system_property_read(pi, NULL, ap_ifname);
  }
  if (ap_ifname[0] == '\0') {
    pi = __system_property_find("wifi.concurrent.interface");
    if (pi != NULL) {
      __system_property_read(pi, NULL, ap_ifname);
    }
  }
  if (ap_ifname[0] == '\0') {
    strlcpy(ap_ifname, "wlan1", sizeof(ap_ifname));
  }
  state.ap_ifindex = if_nametoindex(ap_ifname);

  struct nl_sock *nl_sock_send = nl_socket_alloc();
  if (nl_sock_send == NULL) {
    LOGE("Can't allocate send socket");
    return;
  }

  int nl_err = genl_connect(nl_sock_send);
  if (nl_err < 0) {
    LOGE("Can't connect send socket: %s", nl_geterror(nl_err));
    return;
  }

  int send_fam = genl_ctrl_resolve(nl_sock_send, "nl80211");
  if (send_fam < 0) {
    LOGE("Can't resolve nl80211 family: %s", nl_geterror(send_fam));
    return;
  }

  nl_socket_disable_seq_check(nl_sock_send);

  struct nl_sock *nl_sock_recv = nl_socket_alloc();
  if (nl_sock_recv == NULL) {
    LOGE("Can't allocate receive socket");
    return;
  }

  nl_err = genl_connect(nl_sock_recv);
  if (nl_err < 0) {
    LOGE("Can't connect receive socket: %s", nl_geterror(nl_err));
    return;
  }

  int recv_grp = genl_ctrl_resolve_grp(nl_sock_recv, "cld80211", "oem_msgs");
  if (recv_grp < 0) {
    LOGE("Can't resolve cld80211 oem_msgs group: %s", nl_geterror(recv_grp));
    return;
  }

  nl_err = nl_socket_add_membership(nl_sock_recv, recv_grp);
  if (nl_err < 0) {
    LOGE("Can't join cld80211 oem_msgs group: %s", nl_geterror(nl_err));
    return;
  }

  nl_socket_disable_seq_check(nl_sock_recv);

  struct nl_sock *nl_sock_ap_ctrl = nl_socket_alloc();
  if (nl_sock_send == NULL) {
    LOGE("Can't allocate AP control socket");
    return;
  }

  nl_err = genl_connect(nl_sock_ap_ctrl);
  if (nl_err < 0) {
    LOGE("Can't connect AP control socket: %s", nl_geterror(nl_err));
    return;
  }

  nl_socket_disable_seq_check(nl_sock_ap_ctrl);

  struct nl_sock *nl_sock_ap_event = nl_socket_alloc();
  if (nl_sock_ap_event == NULL) {
    LOGE("Can't allocate AP event socket");
    return;
  }

  nl_err = genl_connect(nl_sock_ap_event);
  if (nl_err < 0) {
    LOGE("Can't connect AP event socket: %s", nl_geterror(nl_err));
    return;
  }

  int mlme_grp = genl_ctrl_resolve_grp(nl_sock_ap_event, "nl80211", "mlme");
  if (mlme_grp < 0) {
    LOGE("Can't resolve nl80211 mlme group: %s", nl_geterror(mlme_grp));
    return;
  }

  nl_err = nl_socket_add_membership(nl_sock_ap_event, mlme_grp);
  if (nl_err < 0) {
    LOGE("Can't join nl80211 mlme group: %s", nl_geterror(nl_err));
    return;
  }

  nl_socket_disable_seq_check(nl_sock_ap_event);

  int *ap_freqs = NULL;
  int ap_freqs_count = (*env)->GetArrayLength(env, apFreqs);
  if (ap_freqs_count > 0) {
    ap_freqs = calloc((size_t)ap_freqs_count, sizeof(int));
    if (ap_freqs == NULL) {
      ap_freqs_count = 0;
      LOGE("Can't allocate array of AP frequencies");
      return;
    }
    (*env)->GetIntArrayRegion(env, apFreqs, 0, ap_freqs_count, ap_freqs);
  }

  uint32_t fft_size = (uint32_t)fftSize;

  state.ap_freqs = ap_freqs;
  state.ap_freqs_count = ap_freqs_count;
  state.fft_size = fft_size;
  state.sock_forward = sock_forward;
  state.ifindex = ifindex;
  state.send_fam = send_fam;
  state.nl_sock_send = nl_sock_send;
  state.nl_sock_recv = nl_sock_recv;
  state.nl_sock_ap_ctrl = nl_sock_ap_ctrl;
  state.nl_sock_ap_event = nl_sock_ap_event;

  state.running = true;
  pthread_create(&state.ap_ctrl_thread, 0, ap_ctrl_thread, NULL);
  pthread_create(&state.scan_thread, 0, scan_thread, NULL);
  pthread_create(&state.forward_thread, 0, forward_thread, NULL);
}

JNIEXPORT void JNICALL
Java_com_example_spectral_1plot_ScanService_stopScan(JNIEnv *env, jobject obj) {
  if (!state.running) {
    return;
  }

  state.running = false;
  pthread_kill(state.forward_thread, SIGINT);
  pthread_kill(state.scan_thread, SIGINT);
  pthread_kill(state.ap_ctrl_thread, SIGINT);
  pthread_join(state.forward_thread, NULL);
  pthread_join(state.scan_thread, NULL);
  pthread_join(state.ap_ctrl_thread, NULL);

  free(state.ap_freqs);
  state.ap_freqs = NULL;

  nl_socket_free(state.nl_sock_ap_event);
  nl_socket_free(state.nl_sock_ap_ctrl);
  nl_socket_free(state.nl_sock_recv);
  nl_socket_free(state.nl_sock_send);

  if (close(state.sock_forward) < 0) {
    LOGW("Can't close forward socket: %s", strerror(errno));
  }

  state.nl_sock_ap_event = NULL;
  state.nl_sock_ap_ctrl = NULL;
  state.nl_sock_recv = NULL;
  state.nl_sock_send = NULL;
}
