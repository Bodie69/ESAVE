package ph.edu.rsu.cajidiocan.smartclassroom;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.util.AttributeSet;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

public final class EnergyChartView extends View {
    private static final int MAX_SAMPLES = 60;
    private final List<Float> samples = new ArrayList<>();
    private final Paint gridPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    public EnergyChartView(Context context) { super(context); init(); }
    public EnergyChartView(Context context, AttributeSet attrs) { super(context, attrs); init(); }
    public EnergyChartView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); init(); }

    private void init() {
        float d = getResources().getDisplayMetrics().density;
        gridPaint.setColor(Color.rgb(226, 232, 240));
        gridPaint.setStrokeWidth(1f * d);
        linePaint.setColor(Color.rgb(11, 110, 243));
        linePaint.setStyle(Paint.Style.STROKE);
        linePaint.setStrokeWidth(2.5f * d);
        linePaint.setStrokeCap(Paint.Cap.ROUND);
        linePaint.setStrokeJoin(Paint.Join.ROUND);
        fillPaint.setColor(Color.argb(28, 11, 110, 243));
        fillPaint.setStyle(Paint.Style.FILL);
        textPaint.setColor(Color.rgb(100, 116, 139));
        textPaint.setTextSize(12f * getResources().getDisplayMetrics().scaledDensity);
    }

    public void addSample(double watts) {
        if (Double.isNaN(watts) || Double.isInfinite(watts) || watts < 0) return;
        if (samples.size() >= MAX_SAMPLES) samples.remove(0);
        samples.add((float) watts);
        invalidate();
    }

    public void clearSamples() {
        samples.clear();
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float w = getWidth();
        float h = getHeight();
        float left = dp(8);
        float right = w - dp(8);
        float top = dp(12);
        float bottom = h - dp(24);

        for (int i = 0; i <= 4; i++) {
            float y = top + (bottom - top) * i / 4f;
            canvas.drawLine(left, y, right, y, gridPaint);
        }

        if (samples.size() < 2) {
            canvas.drawText("Waiting for live PZEM power samples...", left, h / 2f, textPaint);
            return;
        }

        float max = 1f;
        for (float v : samples) max = Math.max(max, v);
        max = Math.max(50f, (float) Math.ceil(max / 50f) * 50f);

        Path line = new Path();
        Path fill = new Path();
        for (int i = 0; i < samples.size(); i++) {
            float x = left + (right - left) * i / (samples.size() - 1f);
            float y = bottom - (samples.get(i) / max) * (bottom - top);
            if (i == 0) {
                line.moveTo(x, y);
                fill.moveTo(x, bottom);
                fill.lineTo(x, y);
            } else {
                line.lineTo(x, y);
                fill.lineTo(x, y);
            }
        }
        fill.lineTo(right, bottom);
        fill.close();
        canvas.drawPath(fill, fillPaint);
        canvas.drawPath(line, linePaint);
        canvas.drawText(String.format(java.util.Locale.US, "0–%.0f W", max), left, h - dp(5), textPaint);
    }

    private float dp(float v) { return v * getResources().getDisplayMetrics().density; }
}
