#include <android/bitmap.h>
#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_TAG "spectral-plot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SPECTRAL_DETECT

enum { MAX_NUM_BINS = 512 };
enum { SPAN_WIDTH = 40 };

struct scan_data {
  int8_t bin_pwr[MAX_NUM_BINS];
  uint16_t bin_pwr_count;
  uint16_t center_freq;
  int32_t tstamp;
};

struct window_avg_data {
  double bin_pwr[MAX_NUM_BINS];
  uint16_t bin_pwr_count;
  uint16_t center_freq;
  int32_t tstamp;
};

#ifdef SPECTRAL_DETECT
static const int thres_min = -100;
#else
static const int thres_min = -80;
#endif
static const int thres_diff = 10;

struct pulse_single {
  double center;
  double bw;
  double pwr;
  int32_t tstamp;
};

static struct pulse_single make_pulse(const struct window_avg_data *data,
                                      const uint16_t bin_start,
                                      const uint16_t bin_end,
                                      const uint16_t bin_peak) {
  const double *const bin_pwr = data->bin_pwr;
  const uint16_t bin_pwr_count = data->bin_pwr_count;
  const uint16_t center_freq = data->center_freq;

  double sum_pwr = 0;
  double sum_prod = 0;
  for (uint16_t bin = bin_start; bin < bin_end; bin++) {
    sum_pwr += bin_pwr[bin];
    sum_prod += bin * bin_pwr[bin];
  }
  double center_bin = sum_prod / sum_pwr;

  double sum_dis = 0;
  for (uint16_t bin = bin_start; bin < bin_end; bin++) {
    sum_dis += pow(bin - center_bin, 2.0) * bin_pwr[bin];
  }
  double bw_bin = 2 * sqrt(sum_dis / sum_pwr);

  return (struct pulse_single){
      .center = (center_bin / bin_pwr_count - 0.5) * SPAN_WIDTH + center_freq,
      .bw = bw_bin / bin_pwr_count * SPAN_WIDTH,
      .pwr = bin_pwr[bin_peak],
      .tstamp = data->tstamp,
  };
}

static uint16_t detect_pulses(const struct window_avg_data *data,
                              struct pulse_single pulses[]) {
  const double *const bin_pwr = data->bin_pwr;
  const uint16_t bin_pwr_count = data->bin_pwr_count;

  uint16_t num_pulses = 0;

  for (uint16_t bin_start = 0, bin_end = 0, bin_peak = 0, bin_next = 0;
       bin_end < bin_pwr_count; bin_start = bin_peak = bin_end = bin_next) {
    while (bin_start > 0 &&
           bin_pwr[bin_start - 1] > bin_pwr[bin_peak] - thres_diff &&
           bin_pwr[bin_start - 1] < bin_pwr[bin_peak]) {
      bin_start--;
    }
    if (bin_start > 0 && bin_pwr[bin_start - 1] >= bin_pwr[bin_peak]) {
      bin_next++;
      continue;
    }

    while (bin_end < bin_pwr_count &&
           bin_pwr[bin_end] > bin_pwr[bin_peak] - thres_diff &&
           bin_pwr[bin_end] <= bin_pwr[bin_peak]) {
      bin_end++;
    }
    bin_next = bin_end;
    if (bin_end < bin_pwr_count && bin_pwr[bin_end] > bin_pwr[bin_peak]) {
      continue;
    }
    if (bin_pwr[bin_peak] <= thres_min) {
      continue;
    }

    while (bin_start < bin_peak && bin_pwr[bin_start] <= thres_min) {
      bin_start++;
    }
    while (bin_end > bin_peak && bin_pwr[bin_end - 1] <= thres_min) {
      bin_end--;
    }
#ifdef SPECTRAL_DETECT
    if (bin_start + 1 >= bin_end) {
#else
    if (bin_start >= bin_end) {
#endif
      continue;
    }

    pulses[num_pulses++] = make_pulse(data, bin_start, bin_end, bin_peak);
  }

  return num_pulses;
}

struct pulse {
  double center;
  double bw;
  double pwr;
  int32_t tstamp_first;
  int32_t tstamp_last;
  int32_t cnt;
  bool matched;
};

static uint16_t match_pulses(const struct pulse_single new_pulses[],
                             const uint16_t new_num_pulses,
                             const uint16_t bin_pwr_count,
                             struct pulse old_pulses[],
                             const uint16_t old_num_pulses,
                             struct pulse pulses[]) {
  static const double thres_freq = 1.0;
  static const double thres_pwr = 3.0;
  static const int32_t thres_time = 150;
  uint16_t num_pulses = 0;

  for (uint16_t new_idx = 0, old_idx = 0; new_idx < new_num_pulses; new_idx++) {
    double center = new_pulses[new_idx].center;
    double bw = new_pulses[new_idx].bw;
    double pwr = new_pulses[new_idx].pwr;
    int32_t tstamp = new_pulses[new_idx].tstamp;

    while (old_idx < old_num_pulses &&
           old_pulses[old_idx].center <= center - thres_freq) {
      old_idx++;
    }

    if (old_idx < old_num_pulses &&
        old_pulses[old_idx].center < center + thres_freq &&
        fabs(old_pulses[old_idx].bw - bw) < thres_freq * 2 &&
        fabs(old_pulses[old_idx].pwr - pwr) < thres_pwr &&
        tstamp < old_pulses[old_idx].tstamp_last + thres_time) {
      double old_center = old_pulses[old_idx].center;
      double old_bw = old_pulses[old_idx].bw;
      double old_pwr = old_pulses[old_idx].pwr;
      int32_t old_cnt = old_pulses[old_idx].cnt;
      center = (center + old_cnt * old_center) / (old_cnt + 1);
      bw = (bw + old_cnt * old_bw) / (old_cnt + 1);
      pwr = (pwr + old_cnt * old_pwr) / (old_cnt + 1);
      pulses[num_pulses++] = (struct pulse){
          .center = center,
          .bw = bw,
          .pwr = pwr,
          .tstamp_first = old_pulses[old_idx].tstamp_first,
          .tstamp_last = tstamp,
          .cnt = old_cnt + 1,
          .matched = false,
      };
      old_pulses[old_idx++].matched = true;
    } else {
      pulses[num_pulses++] = (struct pulse){
          .center = center,
          .bw = bw,
          .pwr = pwr,
          .tstamp_first = tstamp,
          .tstamp_last = tstamp,
          .cnt = 1,
          .matched = false,
      };
    }
  }

  return num_pulses;
}

static uint16_t make565(int red, int green, int blue) {
  return (uint16_t)(((red << 8) & 0xf800) | ((green << 3) & 0x07e0) |
                    ((blue >> 3) & 0x001f));
}

struct plot_data {
  uint16_t pixels[MAX_NUM_BINS];
  uint16_t num_pixels;
  int32_t tstamp;
};

static struct {
  atomic_bool running;
  atomic_bool show_pulses;
  jfieldID bitmap_fid;
  jfieldID elapsed_q1_fid;
  jfieldID elapsed_q2_fid;
  jfieldID elapsed_q3_fid;
  jfieldID center_pos_fid;
  jfieldID center_freq_fid;
  jfieldID span_width_fid;
#ifdef SPECTRAL_DETECT
  jfieldID bt_pwr_fid;
#else
  jfieldID pulse_freq_fid;
#endif
  struct sockaddr_un saddr;
  int sock_fd;
  const char *sock_path;
  struct plot_data *rbuffer;
  size_t rbuffer_capacity;
  size_t rbuffer_size;
  size_t rbuffer_pos;
  uint16_t center_freq;
#ifdef SPECTRAL_DETECT
  double bt_pwr;
#else
  double pulse_freq;
#endif
  sem_t sem;
  pthread_t recv_thread;
} state;

static void handle_sigint(int sig) {}

static void *recv_thread(void *arg) {
  struct sigaction sa = {.sa_handler = handle_sigint};
  sigaction(SIGINT, &sa, NULL);

  enum { MAX_WINDOW_SIZE = 200 };
  static const int32_t max_window_time = 625;
  struct scan_data scans[MAX_WINDOW_SIZE];
  size_t window_start = 0;
  size_t window_size = 0;
  int window_sum[MAX_NUM_BINS] = {0};
  struct pulse pulses[MAX_NUM_BINS] = {0};
  uint16_t num_pulses = 0;
#ifdef SPECTRAL_DETECT
  int32_t prev_tstamp = INT32_MAX;
  enum { NUM_BT_CHANS = 79 };
  enum { NUM_ZB_CHANS = 16 };
  int non_bt_score[NUM_BT_CHANS] = {0};
  int non_zb_score[NUM_ZB_CHANS] = {0};
  int last_bt_chan = -1;
  int bt_score = 0;
  static const int32_t bt_max_window_length = 20000;
  struct bt_pwr_data {
    double pwr_total;
    int32_t length;
  } bt_window[MAX_WINDOW_SIZE];
  size_t bt_window_start = 0;
  size_t bt_window_size = 0;
  double bt_window_sum = 0;
  int32_t bt_window_length = 0;
#endif

  sem_wait(&state.sem);

  while (state.running) {
    sem_post(&state.sem);
    uint8_t samp_buf[1216];
    const ssize_t samp_len = recv(state.sock_fd, samp_buf, sizeof(samp_buf), 0);
    sem_wait(&state.sem);

    if (samp_len < 93) {
      continue;
    }
    if (*(uint32_t *)samp_buf != 0xdeadbeef) {
      continue;
    }

    const int32_t tstamp = *(int32_t *)(samp_buf + 44);
    const uint16_t bin_pwr_count = *(uint16_t *)(samp_buf + 87);
    if (samp_len < 93 + bin_pwr_count || bin_pwr_count > MAX_NUM_BINS) {
      continue;
    }
    const int8_t *bin_pwr = (int8_t *)samp_buf + 93;
    const uint16_t center_freq = *(uint16_t *)(samp_buf + 4);

    while (window_size > 0 &&
           (scans[window_start].bin_pwr_count != bin_pwr_count ||
            scans[window_start].center_freq != center_freq ||
            scans[window_start].tstamp <= tstamp - max_window_time)) {
      const struct scan_data *old = &scans[window_start++];
      window_start %= MAX_WINDOW_SIZE;
      for (uint16_t bin = 0; bin < old->bin_pwr_count; bin++) {
        window_sum[bin] -= old->bin_pwr[bin];
      }
      window_size--;
    }

    size_t window_end = window_start + window_size;
    window_end %= MAX_WINDOW_SIZE;
    struct scan_data *scan_data = &scans[window_end];

    if (window_size > 0 && window_end == window_start) {
      window_start++;
      window_start %= MAX_WINDOW_SIZE;
      for (uint16_t bin = 0; bin < scan_data->bin_pwr_count; bin++) {
        window_sum[bin] -= scan_data->bin_pwr[bin];
      }
      window_size--;
    }

    memcpy(scan_data->bin_pwr, bin_pwr, bin_pwr_count);
    scan_data->bin_pwr_count = bin_pwr_count;
    scan_data->center_freq = center_freq;
    scan_data->tstamp = tstamp;

    for (uint16_t bin = 0; bin < bin_pwr_count; bin++) {
      window_sum[bin] += bin_pwr[bin];
    }
    window_size++;

    struct window_avg_data avg_data;
    for (uint16_t bin = 0; bin < bin_pwr_count; bin++) {
      avg_data.bin_pwr[bin] = window_sum[bin] / (double)window_size;
    }
    avg_data.bin_pwr_count = bin_pwr_count;
    avg_data.center_freq = center_freq;
    avg_data.tstamp = tstamp;

    struct pulse_single new_pulses[MAX_NUM_BINS];
    const uint16_t new_num_pulses = detect_pulses(&avg_data, new_pulses);

    struct pulse old_pulses[MAX_NUM_BINS];
    const uint16_t old_num_pulses = num_pulses;
    memcpy(old_pulses, pulses, old_num_pulses * sizeof(struct pulse));
    num_pulses = match_pulses(new_pulses, new_num_pulses, bin_pwr_count,
                              old_pulses, old_num_pulses, pulses);

#ifdef SPECTRAL_DETECT
    if (tstamp > prev_tstamp) {
      const int32_t elapsed = tstamp - prev_tstamp;

      for (int bt_chan = 0; bt_chan < NUM_BT_CHANS; bt_chan++) {
        if (non_bt_score[bt_chan] > elapsed) {
          non_bt_score[bt_chan] -= elapsed;
        } else {
          non_bt_score[bt_chan] = 0;
        }
      }

      for (int zb_chan = 0; zb_chan < NUM_ZB_CHANS; zb_chan++) {
        if (non_zb_score[zb_chan] > elapsed) {
          non_zb_score[zb_chan] -= elapsed;
        } else {
          non_zb_score[zb_chan] = 0;
        }
      }

      if (bt_score > elapsed) {
        bt_score -= elapsed;
      } else {
        bt_score = 0;
      }
    }
#else
    int32_t max_pulse_length = -1;
    double max_pulse_freq = 0;
#endif

#ifdef SPECTRAL_DETECT
    for (uint16_t pulse_idx = 0; pulse_idx < old_num_pulses; pulse_idx++) {
      if (old_pulses[pulse_idx].matched) {
        continue;
      }

      int32_t length = old_pulses[pulse_idx].tstamp_last -
                       old_pulses[pulse_idx].tstamp_first;
      double center = old_pulses[pulse_idx].center;
      double bw = old_pulses[pulse_idx].bw;
      double pwr = old_pulses[pulse_idx].pwr;
      int bt_chan_center = (int)round(center - 2402);
      int bt_chan_start = (int)round(center - bw / 2 - 2402);
      int bt_chan_end = (int)round(center + bw / 2 - 2402) + 1;

      if (bt_chan_start < 0) {
        bt_chan_start = 0;
      }
      if (bt_chan_end > NUM_BT_CHANS) {
        bt_chan_end = NUM_BT_CHANS;
      }

      if (bw > 2) {
        for (int bt_chan = bt_chan_start; bt_chan < bt_chan_end; bt_chan++) {
          non_bt_score[bt_chan] = 2500;
        }
      } else if (length > 150 && length < 3750 && bw > 0.5 && bw < 1 &&
                 bt_chan_center >= 0 && bt_chan_center < NUM_BT_CHANS &&
                 non_bt_score[bt_chan_center] <= 0 &&
                 bt_chan_center != last_bt_chan) {
        last_bt_chan = bt_chan_center;
        bt_score += length * 100;
        if (bt_score > 2000000) {
          bt_score = 2000000;
        }

        size_t bt_window_end = bt_window_start + bt_window_size;
        bt_window_end %= MAX_WINDOW_SIZE;
        double pwr_total = pwr * length;
        bt_window[bt_window_end].pwr_total = pwr_total;
        bt_window[bt_window_end].length = length;
        bt_window_sum += pwr_total;
        bt_window_length += length;
        bt_window_size++;

        while (bt_window_size > 0 && bt_window_length >= bt_max_window_length) {
          bt_window_sum -= bt_window[bt_window_start].pwr_total;
          bt_window_length -= bt_window[bt_window_start].length;
          bt_window_start++;
          bt_window_start %= MAX_WINDOW_SIZE;
          bt_window_size--;
        }
      }

      if (bw < 1 || bw > 2) {
        for (int bt_chan = bt_chan_start; bt_chan < bt_chan_end; bt_chan++) {
          if (bt_chan % 5 == 3) {
            non_zb_score[bt_chan / 5] = 2500;
          }
        }
      } else if (length > 150 && length < 6250 && bt_chan_center % 5 == 3 &&
                 bt_chan_center / 5 >= 0 && bt_chan_center / 5 < NUM_ZB_CHANS &&
                 non_zb_score[bt_chan_center / 5] <= 0) {
        // LOGI("Chan: %d, Center: %.4f, BW: %.4f, Length: %d",
        //      bt_chan_center / 5 + 11, center, bw, length);
      }
    }
#else
    for (uint16_t pulse_idx = 0; pulse_idx < old_num_pulses; pulse_idx++) {
      int32_t length = old_pulses[pulse_idx].tstamp_last -
                       old_pulses[pulse_idx].tstamp_first;
      double center = old_pulses[pulse_idx].center;
      if (length > max_pulse_length) {
        max_pulse_length = length;
        max_pulse_freq = center;
      }
    }
#endif

    state.center_freq = center_freq;
#ifdef SPECTRAL_DETECT
    prev_tstamp = tstamp;
    state.bt_pwr = bt_score >= 1000000 ? bt_window_sum / bt_window_length : NAN;
#else
    state.pulse_freq = max_pulse_length >= 0 ? max_pulse_freq : NAN;
#endif

    if (state.rbuffer_capacity == 0) {
      continue;
    }

    size_t rbuffer_write_pos = state.rbuffer_pos + state.rbuffer_size;
    rbuffer_write_pos %= state.rbuffer_capacity;
    struct plot_data *plot_data = &state.rbuffer[rbuffer_write_pos];
    plot_data->num_pixels = bin_pwr_count;
    plot_data->tstamp = tstamp;

    for (uint16_t bin = 0; bin < bin_pwr_count; bin++) {
      int8_t pwr = bin_pwr[bin];
      uint16_t pixel = make565(0x80 + pwr, 0x40 + pwr / 2, 0xc0 + pwr / 2);
      plot_data->pixels[bin] = pixel;
    }

    if (state.show_pulses) {
      for (uint16_t pulse_idx = 0; pulse_idx < old_num_pulses; pulse_idx++) {
        if (old_pulses[pulse_idx].matched) {
          continue;
        }

        double center_norm =
            (old_pulses[pulse_idx].center - center_freq) / SPAN_WIDTH + 0.5;
        double bw_norm = old_pulses[pulse_idx].bw / SPAN_WIDTH;
        double center_bin = center_norm * bin_pwr_count;
        double bw_bin = bw_norm * bin_pwr_count;
        int bin_start = (int)round(center_bin - bw_bin / 2);
        int bin_end = (int)round(center_bin + bw_bin / 2) + 1;

        if (bin_start < 0) {
          bin_start = 0;
        }
        if (bin_end > bin_pwr_count) {
          bin_end = bin_pwr_count;
        }

        for (int bin = bin_start; bin < bin_end; bin++) {
          uint16_t pixel = make565(0xff, 0, 0);
          plot_data->pixels[bin] = pixel;
        }
      }

      for (uint16_t pulse_idx = 0; pulse_idx < new_num_pulses; pulse_idx++) {
        double center_norm =
            (new_pulses[pulse_idx].center - center_freq) / SPAN_WIDTH + 0.5;
        double bw_norm = new_pulses[pulse_idx].bw / SPAN_WIDTH;
        double center_bin = center_norm * bin_pwr_count;
        double bw_bin = bw_norm * bin_pwr_count;
        int bin_start = (int)round(center_bin - bw_bin / 2);
        int bin_end = (int)round(center_bin + bw_bin / 2) + 1;

        if (bin_start < 0) {
          bin_start = 0;
        }
        if (bin_end > bin_pwr_count) {
          bin_end = bin_pwr_count;
        }

        for (int bin = bin_start; bin < bin_end; bin++) {
          int8_t pwr = bin_pwr[bin];
          uint16_t pixel = make565(0x80 + pwr, 0xc0 + pwr / 2, 0x40 + pwr / 2);
          plot_data->pixels[bin] = pixel;
        }
      }
    }

    if (state.rbuffer_size < state.rbuffer_capacity) {
      state.rbuffer_size++;
    } else {
      state.rbuffer_pos++;
      state.rbuffer_pos %= state.rbuffer_capacity;
    }
  }

  sem_post(&state.sem);

  return NULL;
}

static void resize_rbuffer(int32_t height) {
  if (height > 0) {
    free(state.rbuffer);
    state.rbuffer = calloc((size_t)height, sizeof(struct plot_data));
    if (state.rbuffer == NULL) {
      LOGE("Can't allocate ring buffer");
      height = 0;
    }
  } else {
    free(state.rbuffer);
    state.rbuffer = NULL;
    height = 0;
  }

  state.rbuffer_capacity = (size_t)height;
  state.rbuffer_size = 0;
  state.rbuffer_pos = 0;
}

static size_t update_plot(const AndroidBitmapInfo *info,
                          uint8_t *const pixels) {
  const size_t num_scans = state.rbuffer_size;

  if (num_scans == 0) {
    return 0;
  } else if (num_scans < info->height) {
    memmove(pixels + num_scans * info->stride, pixels,
            (info->height - num_scans) * info->stride);
  }

  while (state.rbuffer_size > 0) {
    const struct plot_data *plot_data = &state.rbuffer[state.rbuffer_pos++];
    state.rbuffer_pos %= state.rbuffer_capacity;
    state.rbuffer_size--;

    if (state.rbuffer_size >= info->height) {
      continue;
    }

    const uint32_t bin_width = info->width / plot_data->num_pixels;
    uint16_t *ptr = (uint16_t *)(pixels + state.rbuffer_size * info->stride);
    uint16_t *ptr_end = ptr + info->width;

    for (uint16_t idx = 0; idx < plot_data->num_pixels;
         idx++, ptr += bin_width) {
      const uint16_t pixel = plot_data->pixels[idx];

      for (uint32_t i = 0; i < bin_width; i++) {
        ptr[i] = pixel;
      }
    }

    for (; ptr < ptr_end; ptr++) {
      *ptr = 0;
    }
  }

  return num_scans;
}

JNIEXPORT void JNICALL Java_com_example_softsa_PlotView_startPlot(
    JNIEnv *env, jclass cls, jstring sockPath) {
  if (state.running) {
    return;
  }

  state.bitmap_fid =
      (*env)->GetFieldID(env, cls, "plotBitmap", "Landroid/graphics/Bitmap;");
  state.elapsed_q1_fid = (*env)->GetFieldID(env, cls, "elapsedQ1", "J");
  state.elapsed_q2_fid = (*env)->GetFieldID(env, cls, "elapsedQ2", "J");
  state.elapsed_q3_fid = (*env)->GetFieldID(env, cls, "elapsedQ3", "J");
  state.center_pos_fid = (*env)->GetFieldID(env, cls, "centerPos", "F");
  state.center_freq_fid = (*env)->GetFieldID(env, cls, "centerFreq", "I");
  state.span_width_fid = (*env)->GetFieldID(env, cls, "spanWidth", "I");
#ifdef SPECTRAL_DETECT
  state.bt_pwr_fid = (*env)->GetFieldID(env, cls, "bluetoothPower", "D");
#else
  state.pulse_freq_fid = (*env)->GetFieldID(env, cls, "pulseFreq", "D");
#endif

  const char *sock_path = (*env)->GetStringUTFChars(env, sockPath, NULL);
  if (sock_path == NULL) {
    LOGE("Can't get socket path");
    return;
  }

  memset(&state.saddr, 0, sizeof(state.saddr));
  state.saddr.sun_family = AF_UNIX;
  const size_t sun_path_len =
      sizeof(struct sockaddr_un) - offsetof(struct sockaddr_un, sun_path);
  strlcpy(state.saddr.sun_path, sock_path, sun_path_len);

  (*env)->ReleaseStringUTFChars(env, sockPath, sock_path);
  sock_path = NULL;

  int sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sock_fd < 0) {
    LOGE("Can't create socket: %s", strerror(errno));
    return;
  }

  if (bind(sock_fd, (struct sockaddr *)&state.saddr, sizeof(state.saddr)) < 0) {
    LOGE("Can't bind socket: %s", strerror(errno));
    return;
  }

  state.sock_fd = sock_fd;
  state.sock_path = state.saddr.sun_path;

#ifdef SPECTRAL_DETECT
  state.bt_pwr = NAN;
#else
  state.pulse_freq = NAN;
#endif
  sem_init(&state.sem, 0, 1);
  state.running = true;
  pthread_create(&state.recv_thread, 0, recv_thread, NULL);
}

JNIEXPORT void JNICALL Java_com_example_softsa_PlotView_stopPlot(JNIEnv *env,
                                                                 jclass cls) {
  if (!state.running) {
    return;
  }

  state.running = false;
  pthread_kill(state.recv_thread, SIGINT);
  pthread_join(state.recv_thread, NULL);
  sem_destroy(&state.sem);
  resize_rbuffer(0);

  if (close(state.sock_fd) < 0) {
    LOGW("Can't close socket: %s", strerror(errno));
  }

  if (unlink(state.sock_path) < 0) {
    LOGW("Can't unlink socket: %s", strerror(errno));
  }

  state.sock_fd = 0;
  state.sock_path = NULL;
}

JNIEXPORT void JNICALL Java_com_example_softsa_PlotView_configPlot(
    JNIEnv *env, jclass cls, jboolean showPulses) {
  state.show_pulses = showPulses;
}

JNIEXPORT void JNICALL Java_com_example_softsa_PlotView_changeHeight(
    JNIEnv *env, jclass cls, jint height) {
  sem_wait(&state.sem);
  if (height >= 0 && (size_t)height != state.rbuffer_capacity) {
    resize_rbuffer(height);
  }
  sem_post(&state.sem);
}

JNIEXPORT jlong JNICALL Java_com_example_softsa_PlotView_updatePlot(
    JNIEnv *env, jclass cls, jobject view) {
  jobject bitmap = (*env)->GetObjectField(env, view, state.bitmap_fid);
  AndroidBitmapInfo info;
  void *pixels;
  int ret;

  if ((ret = AndroidBitmap_getInfo(env, bitmap, &info)) < 0) {
    LOGE("AndroidBitmap_getInfo() failed ! error=%d", ret);
    return 0;
  }

  if (info.format != ANDROID_BITMAP_FORMAT_RGB_565) {
    LOGE("Bitmap format is not RGB_565 !");
    return 0;
  }

  if ((ret = AndroidBitmap_lockPixels(env, bitmap, &pixels)) < 0) {
    LOGE("AndroidBitmap_lockPixels() failed ! error=%d", ret);
    return 0;
  }

  sem_wait(&state.sem);

  size_t num_scans = update_plot(&info, pixels);

  int64_t tstamp_q0 = INT32_MIN;
  int64_t tstamp_q1 = INT32_MAX;
  int64_t tstamp_q2 = INT32_MAX;
  int64_t tstamp_q3 = INT32_MAX;
  float center_pos = NAN;
  if (state.rbuffer_capacity == info.height) {
    size_t pos = state.rbuffer_pos + info.height - 1;
    pos %= state.rbuffer_capacity;
    size_t num_pixels = state.rbuffer[pos].num_pixels;
    if (num_pixels > 0) {
      tstamp_q0 = state.rbuffer[pos].tstamp;
      size_t used_width = info.width - info.width % num_pixels;
      center_pos = (float)used_width / 2.0f / (float)info.width;
    }
    pos += info.height - info.height / 4;
    pos %= state.rbuffer_capacity;
    if (state.rbuffer[pos].num_pixels > 0) {
      tstamp_q1 = state.rbuffer[pos].tstamp;
    }
    pos += info.height - info.height / 4;
    pos %= state.rbuffer_capacity;
    if (state.rbuffer[pos].num_pixels > 0) {
      tstamp_q2 = state.rbuffer[pos].tstamp;
    }
    pos += info.height - info.height / 4;
    pos %= state.rbuffer_capacity;
    if (state.rbuffer[pos].num_pixels > 0) {
      tstamp_q3 = state.rbuffer[pos].tstamp;
    }
  }
  uint16_t center_freq = state.center_freq;
#ifdef SPECTRAL_DETECT
  double bt_pwr = state.bt_pwr;
#else
  double pulse_freq = state.pulse_freq;
#endif

  sem_post(&state.sem);

  AndroidBitmap_unlockPixels(env, bitmap);

  (*env)->SetLongField(env, view, state.elapsed_q1_fid, tstamp_q0 - tstamp_q1);
  (*env)->SetLongField(env, view, state.elapsed_q2_fid, tstamp_q0 - tstamp_q2);
  (*env)->SetLongField(env, view, state.elapsed_q3_fid, tstamp_q0 - tstamp_q3);
  (*env)->SetFloatField(env, view, state.center_pos_fid, center_pos);
  (*env)->SetIntField(env, view, state.center_freq_fid, center_freq);
  (*env)->SetIntField(env, view, state.span_width_fid, SPAN_WIDTH);
#ifdef SPECTRAL_DETECT
  (*env)->SetDoubleField(env, view, state.bt_pwr_fid, bt_pwr);
#else
  (*env)->SetDoubleField(env, view, state.pulse_freq_fid, pulse_freq);
#endif

  return (jlong)num_scans;
}
