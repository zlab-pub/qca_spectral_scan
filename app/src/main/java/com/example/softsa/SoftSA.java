package com.example.softsa;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.view.View;
import android.view.Window;

import java.io.File;
import java.lang.Double;
import java.lang.Float;
import java.lang.Math;
import java.lang.System;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import java.util.function.Supplier;
import java.util.stream.IntStream;

import com.topjohnwu.superuser.ipc.RootService;

public class SoftSA extends Activity {
  private static final int[] apFreqsAll = {
    2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
    5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825,
  };
  private boolean[] apFreqsSelected = new boolean[apFreqsAll.length];
  private int fftSize = 7;
  private boolean showPulses = false;
  private ScanConnection scanConn;

  private int[] getApFreqs() {
    return IntStream.range(0, apFreqsSelected.length)
      .filter(i -> apFreqsSelected[i]).map(i -> apFreqsAll[i]).toArray();
  }

  private AlertDialog configApFreqsDialog() {
    AlertDialog.Builder builder = new AlertDialog.Builder(this);
    builder.setTitle("AP Frequencies");
    String[] items = Arrays.stream(apFreqsAll)
      .mapToObj(freq -> String.format("%d MHz", freq)).toArray(String[]::new);
    boolean[] checkedItems = apFreqsSelected.clone();
    builder.setMultiChoiceItems(items, checkedItems, (dialog, which, isChecked) -> {
      checkedItems[which] = isChecked;
    });
    builder.setPositiveButton("OK", (dialog, id) -> {
      apFreqsSelected = checkedItems;
      scanConn.config(getApFreqs(), fftSize);
    });
    builder.setNegativeButton("Cancel", null);
    return builder.create();
  }

  private AlertDialog configBinCountDialog() {
    AlertDialog.Builder builder = new AlertDialog.Builder(this);
    builder.setTitle("Bin Count");
    String[] items = IntStream.rangeClosed(2, 9)
      .mapToObj(i -> String.format("%d", 1 << i)).toArray(String[]::new);
    int[] checkedItem = {fftSize - 2};
    builder.setSingleChoiceItems(items, checkedItem[0], (dialog, which) -> {
      checkedItem[0] = which;
    });
    builder.setPositiveButton("OK", (dialog, id) -> {
      fftSize = checkedItem[0] + 2;
      scanConn.config(getApFreqs(), fftSize);
    });
    builder.setNegativeButton("Cancel", null);
    return builder.create();
  }

  private AlertDialog configSpectrogramDialog() {
    AlertDialog.Builder builder = new AlertDialog.Builder(this);
    builder.setTitle("Spectrogram");
    String[] items = {"Show Pulses"};
    boolean[] checkedItems = {showPulses};
    builder.setMultiChoiceItems(items, checkedItems, (dialog, which, isChecked) -> {
      checkedItems[which] = isChecked;
    });
    builder.setPositiveButton("OK", (dialog, id) -> {
      showPulses = checkedItems[0];
      PlotView.configPlot(showPulses);
    });
    builder.setNegativeButton("Cancel", null);
    return builder.create();
  }

  private AlertDialog configDialog() {
    AlertDialog.Builder builder = new AlertDialog.Builder(this);
    builder.setTitle("Configuration");
    String[] items = {
      "AP Frequencies",
      "Bin Count",
      "Spectrogram",
    };
    List<Supplier<AlertDialog>> dialogBuilders = List.of(
      this::configApFreqsDialog,
      this::configBinCountDialog,
      this::configSpectrogramDialog);
    builder.setItems(items, (dialog, which) -> {
      dialogBuilders.get(which).get().show();
    });
    builder.setNegativeButton("Cancel", null);
    return builder.create();
  }

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    requestWindowFeature(Window.FEATURE_NO_TITLE);
    IntStream.range(0, apFreqsAll.length).forEach(i -> {
      apFreqsSelected[i] = (apFreqsAll[i] == 2422 || apFreqsAll[i] == 2462);
    });
    String uuid = UUID.randomUUID().toString();
    String sockPath = new File(getCacheDir(), uuid + ".sock").getAbsolutePath();
    PlotView.configPlot(showPulses);
    PlotView.startPlot(sockPath);
    View view = new PlotView(this);
    setContentView(view);
    scanConn = new ScanConnection();
    Intent scanIntent = new Intent(this, ScanService.class);
    scanIntent.putExtra("com.example.softsa.ap_freqs", getApFreqs());
    scanIntent.putExtra("com.example.softsa.fft_size", fftSize);
    scanIntent.putExtra("com.example.softsa.sock_path", sockPath);
    RootService.bind(scanIntent, scanConn);
    view.setOnClickListener(v -> {
      scanConn.pause();
    });
    view.setOnLongClickListener(v -> {
      configDialog().show();
      return true;
    });
  }

  @Override
  protected void onDestroy() {
    super.onDestroy();
    RootService.unbind(scanConn);
    PlotView.stopPlot();
  }
}

class PlotView extends View {
  static {
    System.loadLibrary("spectral-plot");
  }

  static native void startPlot(String sockPath);

  static native void stopPlot();

  static native void configPlot(boolean showPulses);

  private static native void changeHeight(int height);

  private static native long updatePlot(PlotView view, Bitmap bitmap);

  private Bitmap plotBitmap;
  private final Rect r = new Rect();
  private final Paint rightLargePaint = new Paint();
  private final Paint rightSmallPaint = new Paint();
  private final Paint leftLargePaint = new Paint();
  private final Paint leftSmallPaint = new Paint();
  private final Paint centerSmallPaint = new Paint();
  private final long[] prevDrawTime = new long[60];
  private final long[] prevNumScans = new long[60];
  private int numDrawsMod60 = 0;
  private long elapsedQ1 = 0;
  private long elapsedQ2 = 0;
  private long elapsedQ3 = 0;
  private float centerPos = Float.NaN;
  private int centerFreq = 0;
  private int spanWidth = 0;
  private double bluetoothPower = Double.NaN;
  private double pulseFreq = Double.NaN;

  PlotView(Context context) {
    super(context);
    float density = getResources().getDisplayMetrics().density;
    rightLargePaint.setColor(Color.YELLOW);
    rightLargePaint.setTextSize(20 * density);
    rightLargePaint.setTextAlign(Paint.Align.RIGHT);
    rightSmallPaint.setColor(Color.YELLOW);
    rightSmallPaint.setTextSize(10 * density);
    rightSmallPaint.setTextAlign(Paint.Align.RIGHT);
    leftLargePaint.setColor(Color.YELLOW);
    leftLargePaint.setTextSize(20 * density);
    leftLargePaint.setTextAlign(Paint.Align.LEFT);
    leftSmallPaint.setColor(Color.YELLOW);
    leftSmallPaint.setTextSize(10 * density);
    leftSmallPaint.setTextAlign(Paint.Align.LEFT);
    centerSmallPaint.setColor(Color.YELLOW);
    centerSmallPaint.setTextSize(10 * density);
    centerSmallPaint.setTextAlign(Paint.Align.CENTER);
  }

  @Override
  protected void onSizeChanged(int w, int h, int oldw, int oldh) {
    plotBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.RGB_565);
    changeHeight(h);
    Arrays.fill(prevDrawTime, System.nanoTime());
  }

  @Override
  protected void onDraw(Canvas canvas) {
    long drawTime = System.nanoTime();
    long numScans = updatePlot(this, plotBitmap);
    long elapsedNano60 = Math.max(drawTime - prevDrawTime[numDrawsMod60], 1);
    prevDrawTime[numDrawsMod60] = drawTime;
    prevNumScans[numDrawsMod60] = numScans;
    long numScans60 = Arrays.stream(prevNumScans).sum();
    long scanRate = Math.round(numScans60 / (elapsedNano60 * 1e-9));
    String scanRateText = scanRate + " scans/s";
    canvas.drawBitmap(plotBitmap, 0, 0, null);
    int width = getWidth();
    int height = getHeight();
    canvas.drawText(scanRateText, width, height, rightLargePaint);
    if (elapsedQ1 > 0) {
      String elapsedQ1Text = String.format("%d ms ago", elapsedQ1 / 1000);
      canvas.drawText(elapsedQ1Text, width, height / 4.0f * 1, rightSmallPaint);
    }
    if (elapsedQ2 > 0) {
      String elapsedQ1Text = String.format("%d ms ago", elapsedQ2 / 1000);
      canvas.drawText(elapsedQ1Text, width, height / 4.0f * 2, rightSmallPaint);
    }
    if (elapsedQ3 > 0) {
      String elapsedQ1Text = String.format("%d ms ago", elapsedQ3 / 1000);
      canvas.drawText(elapsedQ1Text, width, height / 4.0f * 3, rightSmallPaint);
    }
    if (!Float.isNaN(centerPos)) {
      String startFreqText = String.format("%d", centerFreq - spanWidth / 2);
      leftSmallPaint.getTextBounds(startFreqText, 0, startFreqText.length(), r);
      canvas.drawText(startFreqText, 0, r.height(), leftSmallPaint);
      String centerFreqText = String.format("%d", centerFreq);
      centerSmallPaint.getTextBounds(centerFreqText, 0, centerFreqText.length(), r);
      canvas.drawText(centerFreqText, width * centerPos, r.height(), centerSmallPaint);
      String endFreqText = String.format("%d MHz", centerFreq + spanWidth / 2);
      rightSmallPaint.getTextBounds(endFreqText, 0, endFreqText.length(), r);
      canvas.drawText(endFreqText, width, r.height(), rightSmallPaint);
    }
    if (!Double.isNaN(bluetoothPower)) {
      String bluetoothText = String.format("Bluetooth: %3.0f dBm", bluetoothPower);
      canvas.drawText(bluetoothText, 0, height, leftLargePaint);
    } else if (!Double.isNaN(pulseFreq)) {
      String pulseText = String.format("Pulse: %.8f MHz", pulseFreq);
      canvas.drawText(pulseText, 0, height, leftLargePaint);
    }
    numDrawsMod60++;
    numDrawsMod60 %= 60;
    invalidate();
  }
}

class ScanService extends RootService implements Handler.Callback {
  static {
    System.loadLibrary("spectral-scan");
  }

  private static native void startScan(int[] apFreqs, int fftSize, String sockPath);

  private static native void stopScan();

  static final int MSG_PAUSE = 0;
  static final int MSG_CONFIG = 1;

  private int[] apFreqs;
  private int fftSize;
  private String sockPath;
  private boolean paused = false;

  @Override
  public IBinder onBind(Intent intent) {
    apFreqs = intent.getIntArrayExtra("com.example.softsa.ap_freqs");
    fftSize = intent.getIntExtra("com.example.softsa.fft_size", 0);
    sockPath = intent.getStringExtra("com.example.softsa.sock_path");
    startScan(apFreqs, fftSize, sockPath);
    Handler h = new Handler(Looper.getMainLooper(), this);
    Messenger m = new Messenger(h);
    return m.getBinder();
  }

  @Override
  public boolean onUnbind(Intent intent) {
    if (!paused) {
      paused = true;
      stopScan();
    }
    return false;
  }

  @Override
  public boolean handleMessage(Message msg) {
    if (msg.what == MSG_PAUSE) {
      if (paused) {
        paused = false;
        startScan(apFreqs, fftSize, sockPath);
      } else {
        paused = true;
        stopScan();
      }
    } else if (msg.what == MSG_CONFIG) {
      Bundle data = msg.getData();
      apFreqs = data.getIntArray("ap_freqs");
      fftSize = data.getInt("fft_size");
      if (!paused) {
        stopScan();
        startScan(apFreqs, fftSize, sockPath);
      }
    }
    return false;
  }
}

class ScanConnection implements ServiceConnection {
  private Messenger m;

  @Override
  public void onServiceConnected(ComponentName name, IBinder service) {
    m = new Messenger(service);
  }

  @Override
  public void onServiceDisconnected(ComponentName name) {
    m = null;
  }

  void pause() {
    if (m == null) {
      return;
    }
    Message msg = Message.obtain(null, ScanService.MSG_PAUSE);
    try {
      m.send(msg);
    } catch (RemoteException e) {
      e.printStackTrace();
    } finally {
      msg.recycle();
    }
  }

  void config(int[] apFreqs, int fftSize) {
    if (m == null) {
      return;
    }
    Message msg = Message.obtain(null, ScanService.MSG_CONFIG);
    Bundle data = new Bundle();
    data.putIntArray("ap_freqs", apFreqs);
    data.putInt("fft_size", fftSize);
    msg.setData(data);
    try {
      m.send(msg);
    } catch (RemoteException e) {
      e.printStackTrace();
    } finally {
      msg.recycle();
    }
  }
}
