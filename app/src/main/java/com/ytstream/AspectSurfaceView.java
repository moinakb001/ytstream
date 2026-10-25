package com.ytstream;

import android.content.Context;
import android.util.AttributeSet;
import android.view.SurfaceView;

/** SurfaceView locked to 16:9; the native preview renders letterboxed into it. */
public class AspectSurfaceView extends SurfaceView {
    public AspectSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = MeasureSpec.getSize(widthMeasureSpec);
        setMeasuredDimension(w, w * 9 / 16);
    }
}
