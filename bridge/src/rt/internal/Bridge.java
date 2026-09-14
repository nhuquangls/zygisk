package rt.internal;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * Android input only. Reflection keeps this small helper independent of an SDK jar.
 *
 * Sniper co-aim controller. The game owns acquisition/range and supplies its
 * current aim-assist target. A rising game-assist signal starts one short,
 * high-rate residual correction; reaching the target latches the controller
 * until a genuinely new trigger, so it cannot repeatedly hunt around the aim.
 */
public final class Bridge implements Runnable, InvocationHandler {
    private static Bridge instance;
    private final Class<?> unity, activityClass, viewClass, windowClass, callbackClass;
    private final Object handler;
    private final Method post, uptime, action, pointerCount, pointerId, pointerX, pointerY,
            rotation, getWindowManager, getDefaultDisplay;
    private Object player, activity, window, original, proxy;
    private boolean stopped, realTouch, calibrated, layoutFlipped;
    private long lastTouch, nextEngage, fingerGrace, downSent, cycleTarget, pendingTarget;
    private long queuedTarget, queuedUntil;
    private int layout, rotationCache = -1, fingerId = -1, learnLogs, lastFrame = -1;
    private int mode, changeFrames, missingFrames;
    private boolean engaged, awaitingLearn, previousDoing;
    private float fingerX, fingerY, carryX, carryY;
    private final float[] sample = new float[8];

    private static final int ACT_DOWN = 1, ACT_MOVE = 2, ACT_UP = 3, ACT_RESET = 4;
    private static final int ARMED = 0, ACTIVE = 1, LATCHED = 2;
    private static final int ANCHOR_X = 1943, ANCHOR_Y = 736;
    // Small increments at 8ms cadence complement the game's coarse pull.
    private static final float GAIN = 0.20f, MAX_STEP = 4f;
    private static final float NEAR_RADIUS = 48f, NEAR_FLOOR = 0.45f;
    private static final float SETTLE_PX = 3.5f;
    private static final int TARGET_CHANGE_FRAMES = 2, MISSING_LATCH_FRAMES = 8;
    private static final float SCOPE_CLOSED_PROJECTION = 2.30f;
    private static final long TRIGGER_BUFFER_MS = 300;
    private static final float DRIFT_MAX = 380f, LEARN_SLACK = 60f;

    private static native long read(float[] output, int width, int height, long locked);
    private static native void status(int code);
    private static native boolean active();
    private static native boolean apply(int action, int x, int y);
    private static native boolean calibrate(int rotation, int layout);

    private Bridge(ClassLoader appLoader) throws Exception {
        unity = appLoader.loadClass("com.unity3d.player.UnityPlayer");
        activityClass = Class.forName("android.app.Activity");
        viewClass = Class.forName("android.view.View");
        windowClass = Class.forName("android.view.Window");
        callbackClass = Class.forName("android.view.Window$Callback");
        Class<?> looper = Class.forName("android.os.Looper"), hc = Class.forName("android.os.Handler");
        handler = hc.getConstructor(looper).newInstance(looper.getMethod("getMainLooper").invoke(null));
        post = hc.getMethod("postDelayed", Runnable.class, long.class);
        uptime = Class.forName("android.os.SystemClock").getMethod("uptimeMillis");
        Class<?> motion = Class.forName("android.view.MotionEvent");
        action = motion.getMethod("getActionMasked");
        pointerCount = motion.getMethod("getPointerCount");
        pointerId = motion.getMethod("getPointerId", int.class);
        pointerX = motion.getMethod("getX", int.class);
        pointerY = motion.getMethod("getY", int.class);
        Class<?> display = Class.forName("android.view.Display");
        rotation = display.getMethod("getRotation");
        Class<?> manager = Class.forName("android.view.WindowManager");
        getWindowManager = activityClass.getMethod("getWindowManager");
        getDefaultDisplay = manager.getMethod("getDefaultDisplay");
    }

    public static synchronized boolean start(ClassLoader appLoader) {
        if (instance != null) return true;
        try {
            Bridge b = new Bridge(appLoader);
            if (!(Boolean)b.post.invoke(b.handler, b, 0L)) return false;
            instance = b;
            return true;
        } catch (Exception e) {
            log(e);
            return false;
        }
    }

    private static void log(Exception e) {
        try { Class.forName("android.util.Log").getMethod("e", String.class, String.class, Throwable.class)
            .invoke(null, "SysInput", "input helper failed", e); }
        catch (Exception ignored) { }
    }
    private static void info(String message) {
        try { Class.forName("android.util.Log").getMethod("i", String.class, String.class)
            .invoke(null, "SysInput", message); }
        catch (Exception ignored) { }
    }

    private long now() throws Exception { return (Long)uptime.invoke(null); }

    private void release(long time) {
        if (engaged || awaitingLearn) {
            apply(ACT_UP, 0, 0);
            fingerGrace = time + 150;
        }
        engaged = false;
        awaitingLearn = false;
        carryX = carryY = 0;
    }

    private void arm(boolean blockCurrentTrigger) {
        mode = ARMED;
        cycleTarget = pendingTarget = 0;
        changeFrames = missingFrames = 0;
        previousDoing = blockCurrentTrigger;
    }

    private void activate(long id) {
        mode = ACTIVE;
        cycleTarget = id;
        pendingTarget = 0;
        queuedTarget = queuedUntil = 0;
        changeFrames = missingFrames = 0;
        previousDoing = true;
        info("co-aim ACTIVE target=0x" + Long.toHexString(id));
    }

    private void latch(long time) {
        release(time);
        mode = LATCHED;
        queuedTarget = queuedUntil = 0;
        pendingTarget = 0;
        changeFrames = missingFrames = 0;
        info("co-aim LATCHED target=0x" + Long.toHexString(cycleTarget));
    }

    private void cancel(long time) {
        release(time);
        arm(true);
        queuedTarget = queuedUntil = 0;
        nextEngage = Math.max(nextEngage, time + 180);
    }

    private void detach(long time) throws Exception {
        cancel(time);
        if (window != null && windowClass.getMethod("getCallback").invoke(window) == proxy)
            windowClass.getMethod("setCallback", callbackClass).invoke(window, original);
        window = original = proxy = player = activity = null;
        realTouch = false;
    }

    /** True for events produced by the injected contact; they must not gate the loop. */
    private boolean isInjected(Object event, long time) throws Exception {
        if ((Integer)pointerCount.invoke(event) != 1) return false;
        int id = (Integer)pointerId.invoke(event, 0);
        if (id == fingerId && fingerId >= 0) {
            if (engaged || awaitingLearn || time < fingerGrace) {
                if (awaitingLearn && (Integer)action.invoke(event) == 0) awaitingLearn = false;
                int kind = (Integer)action.invoke(event);
                if ((kind == 1 || kind == 3) && engaged) {
                    // The contact was lifted or cancelled somewhere else.
                    engaged = false;
                    fingerGrace = time + 150;
                    nextEngage = Math.max(nextEngage, time + 250);
                }
                return true;
            }
            return false;
        }
        if (engaged || awaitingLearn) {
            if (fingerId >= 0 || (Integer)action.invoke(event) != 0) return false;
            float x = (Float)pointerX.invoke(event, 0);
            float y = (Float)pointerY.invoke(event, 0);
            if (Math.abs(x - ANCHOR_X) <= LEARN_SLACK && Math.abs(y - ANCHOR_Y) <= LEARN_SLACK) {
                fingerId = id;
                awaitingLearn = false;
                if (learnLogs++ == 0) info("contact " + id + " at " + (int)x + "," + (int)y);
                return true;
            }
            // Our own DOWN landed away from the anchor: the display-to-panel
            // layout is wrong. Flip it once, drop the contact and retry.
            info("contact landed at " + (int)x + "," + (int)y + "; layout " + layout);
            apply(ACT_UP, 0, 0);
            fingerGrace = time + 150;
            engaged = false;
            awaitingLearn = false;
            if (layoutFlipped) {
                nextEngage = Long.MAX_VALUE;
                status(19);
            } else {
                layoutFlipped = true;
                layout ^= 1;
                calibrated = calibrate(rotationCache, layout);
                nextEngage = time + 200;
            }
            return true;
        }
        return false;
    }

    @Override public Object invoke(Object object, Method method, Object[] args) throws Throwable {
        String name = method.getName();
        // Observe every touch; the injected contact is recognized and passed
        // through unchanged so it never interrupts the controller.
        if (name.equals("dispatchTouchEvent") && args != null && args.length == 1) {
            long time = now();
            if (!isInjected(args[0], time)) {
                cancel(time);
                int kind = (Integer)action.invoke(args[0]);
                realTouch = kind != 1 && kind != 3;
                lastTouch = time;
            }
        } else if (name.equals("onWindowFocusChanged") && Boolean.FALSE.equals(args[0])) {
            cancel(now());
            realTouch = false;
        }
        try { return method.invoke(original, args); }
        catch (InvocationTargetException e) { throw e.getCause(); }
    }

    @Override public void run() {
        long delay = 8;
        try {
            long time = now();
            if (stopped || !active()) { stopped = true; detach(time); return; }
            Object current = unity.getField("currentActivity").get(null);
            Object currentPlayer = unity.getField("currentPlayer").get(null);
            if (current != activity || currentPlayer != player) {
                detach(time);
                if (current == null || currentPlayer == null) { delay = 500; return; }
                activity = current;
                player = currentPlayer;
                window = activityClass.getMethod("getWindow").invoke(activity);
                original = windowClass.getMethod("getCallback").invoke(window);
                proxy = Proxy.newProxyInstance(callbackClass.getClassLoader(), new Class<?>[]{callbackClass}, this);
                windowClass.getMethod("setCallback", callbackClass).invoke(window, proxy);
                lastTouch = time;
                status(7); // Helper is installed, waiting for a valid scene.
            }
            if (windowClass.getMethod("getCallback").invoke(window) != proxy || realTouch || time-lastTouch < 500 ||
                !(Boolean)viewClass.getMethod("hasWindowFocus").invoke(player) ||
                (Boolean)activityClass.getMethod("isFinishing").invoke(activity) ||
                (Boolean)activityClass.getMethod("isDestroyed").invoke(activity)) {
                cancel(time);
                status(13);
                return;
            }
            int width = (Integer)viewClass.getMethod("getWidth").invoke(player);
            int height = (Integer)viewClass.getMethod("getHeight").invoke(player);
            // The tested touch anchor belongs to this device/HUD. Other viewports stay idle.
            if (width != 2944 || height != 1840) { cancel(time); status(10); return; }
            if (!calibrated) {
                int r = (Integer)rotation.invoke(getDefaultDisplay.invoke(getWindowManager.invoke(activity)));
                if (r != rotationCache) { rotationCache = r; calibrated = calibrate(r, layout); }
                if (!calibrated) { cancel(time); status(10); return; }
            }
            if (!engaged && fingerId >= 0 && time >= fingerGrace) fingerId = -1;
            if (awaitingLearn && time - downSent > 220) { cancel(time); status(18); return; }
            if (queuedTarget != 0 && time > queuedUntil) queuedTarget = queuedUntil = 0;
            long requested = mode == ARMED ? queuedTarget : 0;
            long id = read(sample, width, height, requested);
            if (sample[7] < .5f) {
                release(time);
                arm(true);
                queuedTarget = queuedUntil = 0;
                status(12);
                return;
            }
            int frame = (int)sample[3];
            boolean fresh = frame != lastFrame || lastFrame < 0;
            lastFrame = frame;
            boolean doing = sample[4] > .5f;
            boolean sniperReady = sample[5] > .5f;
            if (!sniperReady) {
                // Game assist can finish before the scope-open animation reaches
                // its final projection. Remember that short edge and consume it
                // only after the sniper scope is fully open.
                boolean closingPreviousScope = mode != ARMED || cycleTarget != 0;
                if (fresh && doing && id != 0) {
                    queuedTarget = id;
                    queuedUntil = time + TRIGGER_BUFFER_MS;
                } else if (sample[2] <= SCOPE_CLOSED_PROJECTION && closingPreviousScope) {
                    queuedTarget = queuedUntil = 0;
                }
                release(time);
                arm(false);
                status(15);
                return;
            }
            if (!fresh) return;

            if (mode == ARMED) {
                boolean buffered = queuedTarget != 0 && time <= queuedUntil && id == queuedTarget;
                if (!buffered && (!doing || previousDoing || id == 0)) {
                    previousDoing = doing;
                    status(15);
                    return;
                }
                activate(id);
            } else if (id != 0 && id != cycleTarget) {
                // Never steer against a half-switched pointer. A stable game
                // target change is itself a new acquisition trigger.
                release(time);
                if (pendingTarget == id) ++changeFrames;
                else { pendingTarget = id; changeFrames = 1; }
                if (changeFrames < TARGET_CHANGE_FRAMES) { status(12); return; }
                if (!doing) {
                    status(mode == LATCHED ? 16 : 12);
                    return;
                }
                activate(id);
            } else {
                pendingTarget = 0;
                changeFrames = 0;
            }

            previousDoing = doing;
            if (mode == LATCHED) { status(16); return; }
            if (id == 0) {
                release(time);
                if (++missingFrames >= MISSING_LATCH_FRAMES) latch(time);
                status(12);
                return;
            }
            missingFrames = 0;

            float response = 2.5f * sample[2] / 5.671282f;
            if (!(response >= .25f && response <= 20f)) { release(time); arm(true); return; }
            double distance = Math.hypot(sample[0], sample[1]);
            if (distance <= SETTLE_PX) {
                latch(time);
                status(16);
                return;
            }
            if (!engaged) {
                if (time < nextEngage || time < fingerGrace) return;
                if (!apply(ACT_DOWN, ANCHOR_X, ANCHOR_Y)) { nextEngage = time + 200; status(18); return; }
                engaged = true;
                awaitingLearn = true;
                downSent = time;
                fingerX = ANCHOR_X;
                fingerY = ANCHOR_Y;
                carryX = carryY = 0;
                status(14);
                return;
            }
            if (Math.hypot(fingerX - ANCHOR_X, fingerY - ANCHOR_Y) > DRIFT_MAX) {
                // Finger ran far from the anchor: lift and re-place, like a player.
                release(time);
                status(14);
                return;
            }

            // Proportional residual step, eased near the target. Prediction is
            // already included in sample[] from filtered world-space velocity.
            float near = distance < NEAR_RADIUS ? (float)(distance / NEAR_RADIUS) : 1f;
            float gain = GAIN * (NEAR_FLOOR + (1f - NEAR_FLOOR) * near);
            float vx = sample[0] / response * gain;
            float vy = sample[1] / response * gain;
            double v = Math.hypot(vx, vy);
            if (v > MAX_STEP) { vx *= MAX_STEP / v; vy *= MAX_STEP / v; }
            carryX += vx;
            carryY += vy;
            int mx = Math.round(carryX), my = Math.round(carryY);
            if (mx != 0 || my != 0) {
                fingerX += mx;
                fingerY += my;
                if (!apply(ACT_MOVE, Math.round(fingerX), Math.round(fingerY))) {
                    status(18);
                    return;
                }
                carryX -= mx;
                carryY -= my;
            }
            status(14);
        } catch (Exception e) {
            log(e);
            // A helper error stops new requests and restores the original callback.
            stopped = true;
            try { detach(now()); } catch (Exception ignored) { }
            status(9);
        } finally {
            if (!stopped) {
                try { post.invoke(handler, this, delay); }
                catch (Exception e) { stopped = true; status(9); }
            }
        }
    }
}
