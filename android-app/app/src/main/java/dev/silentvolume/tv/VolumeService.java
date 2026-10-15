package dev.silentvolume.tv;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

public final class VolumeService extends Service {
    private static final String CHANNEL = "volume_receiver";
    private static final int NOTIFICATION_ID = 1;
    static volatile boolean active;
    static volatile int listeningPort = -1;
    static volatile String error;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private VolumeHttpServer server;
    private VolumeOverlay overlay;
    private int configuredPort;
    private String configuredToken;
    private int generation;

    static void start(Context context) {
        context.startForegroundService(new Intent(context, VolumeService.class));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationChannel channel = new NotificationChannel(CHANNEL,
                getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW);
        channel.setSound(null, null);
        channel.enableVibration(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
        // Promote immediately, before binding sockets or reading the access token.
        startForeground(NOTIFICATION_ID, notification(getString(R.string.status_starting)));
        active = true;
        error = null;
        listeningPort = -1;
        overlay = new VolumeOverlay(this, mainHandler);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            Settings settings = new Settings(this);
            overlay.setOnLeft(settings.overlayOnLeft());
            overlay.setScalePercent(settings.overlayScalePercent());
            if (!settings.overlay()) overlay.hide();
            if (!settings.enabled()) {
                stopSelf();
                return START_NOT_STICKY;
            }
            int port = settings.port();
            String token = settings.token();
            if (server == null || !server.isRunning() || port != configuredPort || !token.equals(configuredToken)) {
                restartServer(port, token);
            }
        } catch (RuntimeException e) {
            Log.e("SilentVolumeTV", "Cannot initialize receiver", e);
            error = "Cannot initialize receiver (" + e.getClass().getSimpleName() + ")";
            updateNotification();
        }
        // Android can recreate the receiver after process death with a null intent.
        return START_STICKY;
    }

    private void restartServer(int port, String token) {
        final int currentGeneration = ++generation;
        if (server != null) server.close();
        configuredPort = port;
        configuredToken = token;
        listeningPort = -1;
        error = null;
        server = new VolumeHttpServer(port,
                new VolumeApi(token, AndroidAudioBackend.controller(this),
                        () -> mainHandler.post(() -> {
                            if (currentGeneration != generation || !active) return;
                            try {
                                if (new Settings(this).overlay()) {
                                    // Read at display time so concurrent commands cannot show an older value.
                                    overlay.show(AndroidAudioBackend.controller(this).status());
                                } else {
                                    overlay.hide();
                                }
                            } catch (RuntimeException e) {
                                Log.w("SilentVolumeTV", "Cannot update volume overlay", e);
                            }
                        })),
                () -> mainHandler.post(() -> {
                    // Old listeners may finish closing after a reconfiguration or service stop.
                    if (currentGeneration != generation || !active) return;
                    listeningPort = server.listeningPort;
                    error = server.error;
                    if (error != null) Log.e("SilentVolumeTV", error);
                    else if (listeningPort > 0) Log.i("SilentVolumeTV", "Listening on port " + listeningPort);
                    updateNotification();
                }));
        server.start();
        updateNotification();
    }

    private Notification notification(String text) {
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class), PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_volume)
                .setContentTitle(getString(R.string.notification_title))
                .setContentText(text)
                .setContentIntent(open)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setSound(null)
                .setCategory(Notification.CATEGORY_SERVICE)
                .build();
    }

    private void updateNotification() {
        String text = error != null ? getString(R.string.status_error, error)
                : listeningPort > 0 ? getString(R.string.notification_text, listeningPort)
                : getString(R.string.status_starting);
        getSystemService(NotificationManager.class).notify(NOTIFICATION_ID, notification(text));
    }

    @Override
    public void onDestroy() {
        ++generation;
        active = false;
        listeningPort = -1;
        error = null;
        if (server != null) server.close();
        if (overlay != null) overlay.close();
        mainHandler.removeCallbacksAndMessages(null);
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
