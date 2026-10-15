package dev.silentvolume.tv;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.RadioGroup;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.util.Collections;
import java.util.Enumeration;
import java.util.Set;
import java.util.TreeSet;
import java.util.Date;
import java.text.DateFormat;

public final class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private dev.silentvolume.tv.Settings settings;
    private VolumeController controller;
    private TextView status;
    private TextView bootStatus;
    private TextView overlayStatus;
    private TextView volume;
    private TextView address;
    private TextView token;
    private EditText port;
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            refreshStatus();
            handler.postDelayed(this, 2000);
        }
    };

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        settings = new dev.silentvolume.tv.Settings(this);
        controller = AndroidAudioBackend.controller(this);
        status = findViewById(R.id.status);
        bootStatus = findViewById(R.id.boot_status);
        overlayStatus = findViewById(R.id.overlay_status);
        volume = findViewById(R.id.volume);
        address = findViewById(R.id.address);
        token = findViewById(R.id.token);
        port = findViewById(R.id.port);
        port.setText(String.valueOf(settings.port()));
        safely(() -> token.setText(settings.token()));

        findViewById(R.id.start).setOnClickListener(view -> safely(() -> {
            settings.enabled(true);
            VolumeService.start(this);
            refreshStatus();
        }));
        findViewById(R.id.stop).setOnClickListener(view -> safely(() -> {
            settings.enabled(false);
            stopService(new Intent(this, VolumeService.class));
            refreshStatus();
        }));
        CheckBox autostart = findViewById(R.id.autostart);
        autostart.setChecked(settings.autostart());
        autostart.setOnCheckedChangeListener((button, checked) -> safely(() -> settings.autostart(checked)));
        CheckBox showOverlay = findViewById(R.id.show_overlay);
        showOverlay.setChecked(settings.overlay());
        showOverlay.setOnCheckedChangeListener((button, checked) -> safely(() -> {
            settings.overlay(checked);
            if (settings.enabled()) VolumeService.start(this);
            refreshStatus();
        }));
        RadioGroup overlayPosition = findViewById(R.id.overlay_position);
        overlayPosition.check(settings.overlayOnLeft() ? R.id.overlay_left : R.id.overlay_right);
        overlayPosition.setOnCheckedChangeListener((group, checkedId) -> safely(() -> {
            settings.overlayOnLeft(checkedId == R.id.overlay_left);
            if (settings.enabled()) VolumeService.start(this);
        }));
        RadioGroup overlaySize = findViewById(R.id.overlay_size);
        int size = settings.overlayScalePercent();
        overlaySize.check(size == 75 ? R.id.overlay_size_75 : size == 125 ? R.id.overlay_size_125
                : size == 150 ? R.id.overlay_size_150 : R.id.overlay_size_100);
        overlaySize.setOnCheckedChangeListener((group, checkedId) -> safely(() -> {
            int percent = checkedId == R.id.overlay_size_75 ? 75 : checkedId == R.id.overlay_size_125 ? 125
                    : checkedId == R.id.overlay_size_150 ? 150 : 100;
            settings.overlayScalePercent(percent);
            if (settings.enabled()) VolumeService.start(this);
        }));
        findViewById(R.id.allow_overlay).setOnClickListener(view -> {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION));
            } catch (ActivityNotFoundException e) {
                Toast.makeText(this, R.string.overlay_settings_unavailable, Toast.LENGTH_LONG).show();
            }
        });
        findViewById(R.id.save).setOnClickListener(view -> savePort());
        findViewById(R.id.rotate_token).setOnClickListener(view -> safely(() -> {
            token.setText(settings.rotateToken());
            if (settings.enabled()) VolumeService.start(this);
            Toast.makeText(this, R.string.token_rotated, Toast.LENGTH_SHORT).show();
        }));
        findViewById(R.id.volume_up).setOnClickListener(view -> adjust(1));
        findViewById(R.id.volume_down).setOnClickListener(view -> adjust(-1));
        findViewById(R.id.battery_settings).setOnClickListener(view -> {
            try {
                startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            } catch (ActivityNotFoundException e) {
                Toast.makeText(this, R.string.settings_unavailable, Toast.LENGTH_LONG).show();
            }
        });
        findViewById(R.id.start).requestFocus();
    }

    private void savePort() {
        int value;
        try {
            value = Integer.parseInt(port.getText().toString().trim());
            if (value < 1024 || value > 65535) throw new NumberFormatException();
        } catch (NumberFormatException e) {
            port.setError(getString(R.string.port_invalid));
            return;
        }
        final int newPort = value;
        safely(() -> {
            settings.port(newPort);
            if (settings.enabled()) VolumeService.start(this);
            Toast.makeText(this, R.string.port_saved, Toast.LENGTH_SHORT).show();
            refreshStatus();
        });
    }

    private void adjust(int direction) {
        safely(() -> {
            controller.adjust(direction);
            refreshStatus();
        });
    }

    private void safely(Runnable action) {
        try {
            action.run();
        } catch (UnsupportedOperationException e) {
            Toast.makeText(this, R.string.fixed_volume, Toast.LENGTH_LONG).show();
        } catch (RuntimeException e) {
            Toast.makeText(this, getString(R.string.command_error, e.getClass().getSimpleName()), Toast.LENGTH_LONG).show();
        }
    }

    private void refreshStatus() {
        overlayStatus.setText(!settings.overlay() ? R.string.overlay_off
                : Settings.canDrawOverlays(this) ? R.string.overlay_ready : R.string.overlay_permission_needed);
        long bootTime = settings.lastBootTime();
        if (bootTime == 0) {
            bootStatus.setText(R.string.boot_not_received);
        } else {
            String result = settings.lastBootResult();
            int message = "requested".equals(result) ? R.string.boot_requested
                    : "disabled".equals(result) ? R.string.boot_disabled : R.string.boot_failed;
            bootStatus.setText(getString(message, DateFormat.getDateTimeInstance().format(new Date(bootTime))));
        }
        if (VolumeService.error != null) {
            status.setText(getString(R.string.status_error, VolumeService.error));
        } else if (VolumeService.listeningPort > 0) {
            status.setText(getString(R.string.status_running, VolumeService.listeningPort));
        } else {
            status.setText(VolumeService.active ? R.string.status_starting : R.string.status_stopped);
        }
        try {
            VolumeController.State state = controller.status();
            volume.setText(state.fixed ? getString(R.string.fixed_volume)
                    : getString(R.string.volume_status, state.volume, state.max, String.valueOf(state.muted)));
        } catch (RuntimeException e) {
            volume.setText(getString(R.string.command_error, e.getClass().getSimpleName()));
        }
        Set<String> urls = new TreeSet<>();
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            if (interfaces != null) {
                for (NetworkInterface network : Collections.list(interfaces)) {
                    if (!network.isUp() || network.isLoopback()) continue;
                    for (InetAddress ip : Collections.list(network.getInetAddresses())) {
                        if (ip instanceof Inet4Address && !ip.isLoopbackAddress() && !ip.isLinkLocalAddress()) {
                            urls.add("http://" + ip.getHostAddress() + ":" + settings.port());
                        }
                    }
                }
            }
        } catch (SocketException ignored) { }
        address.setText(urls.isEmpty() ? getString(R.string.no_address) : String.join("\n", urls));
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Launching the app after a force-stop explicitly permits operation again.
        if (settings.enabled()) safely(() -> VolumeService.start(this));
        handler.post(refresh);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresh);
        super.onPause();
    }
}
