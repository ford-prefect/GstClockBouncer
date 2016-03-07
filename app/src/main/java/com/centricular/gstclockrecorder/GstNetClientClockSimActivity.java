/*
 * Copyright (C) 2016 Arun Raghavan <arun@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

package com.centricular.gstclockrecorder;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Environment;
import android.support.design.widget.Snackbar;
import android.support.v7.app.AppCompatActivity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;

import org.freedesktop.gstreamer.GStreamer;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public class GstNetClientClockSimActivity extends AppCompatActivity {
    private static native boolean nativeClassInit();
    private native boolean nativeStart(String addr, int port, int provider_port, String file_path);
    private native void nativeStop();

    private long native_app_data;
    boolean isStarted;
    WifiManager.WifiLock wlock;

    static {
        System.loadLibrary("gstreamer_android");
        System.loadLibrary("gstnetclientclocksim");
        nativeClassInit();
    }

    private static String getFilePath() {
        SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd-HH:mm:ss.SSS", Locale.US);

        return Environment.getExternalStorageDirectory().getPath() +
                "/gstnetclientclocksim-" +
                sdf.format(new Date()) +
                ".txt";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_clocksim);

        try {
            GStreamer.init(this);
        } catch (Exception e) {
            e.printStackTrace();
        }

        isStarted = false;

        WifiManager wm = (WifiManager) getSystemService(Context.WIFI_SERVICE);
        wlock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF,
                String.valueOf(R.string.app_name));

        wlock.acquire();

        Button start = (Button) findViewById(R.id.sim_start);
        Button stop = (Button) findViewById(R.id.sim_stop);

        start.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Button start = (Button) v;
                Button stop = (Button) findViewById(R.id.sim_stop);

                EditText ip_address = (EditText) findViewById(R.id.sim_bouncer_addr);
                EditText port = (EditText) findViewById(R.id.sim_bouncer_port);
                EditText provider_port = (EditText) findViewById(R.id.sim_provider_port);

                boolean success = nativeStart(ip_address.getText().toString(),
                        Integer.parseInt(port.getText().toString()),
                        Integer.parseInt(provider_port.getText().toString()),
                        getFilePath());

                if (success) {
                    ((GstNetClientClockSimActivity) v.getContext()).isStarted = true;

                    start.setEnabled(false);
                    stop.setEnabled(true);
                } else {
                    Snackbar.make(v, "Initialisation failed", Snackbar.LENGTH_LONG).show();
                }
            }
        });

        stop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Button start = (Button) findViewById(R.id.sim_start);;
                Button stop = (Button) v;

                ((GstNetClientClockSimActivity) v.getContext()).isStarted = false;

                nativeStop();

                stop.setEnabled(false);
                start.setEnabled(true);
            }
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();

        if (isStarted)
            nativeStop();

        this.wlock.release();
    }
}