package ph.edu.rsu.cajidiocan.smartclassroom;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

public final class SplashActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable openMain = () -> {
        startActivity(new Intent(this, MainActivity.class));
        overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
        finish();
    };

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Shown on every Android version so the app name and campus text are always visible.
        // On Android 12+ the system splash (SplashTheme) uses the same navy and hands off to this
        // screen without a colour change.
        setContentView(R.layout.activity_splash);
        handler.postDelayed(openMain, 1400L);
    }

    @Override protected void onDestroy() {
        handler.removeCallbacks(openMain);
        super.onDestroy();
    }
}
