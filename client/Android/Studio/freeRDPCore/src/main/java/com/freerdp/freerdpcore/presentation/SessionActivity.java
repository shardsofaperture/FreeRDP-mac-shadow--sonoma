/*
   Android Session Activity

   Copyright 2013 Thincast Technologies GmbH, Author: Martin Fleisz

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
   If a copy of the MPL was not distributed with this file, You can obtain one at
   http://mozilla.org/MPL/2.0/.
 */

package com.freerdp.freerdpcore.presentation;

import android.app.AlertDialog;
import android.app.Dialog;
import android.app.ProgressDialog;
import android.app.UiModeManager;
import android.content.BroadcastReceiver;
import androidx.core.content.ContextCompat;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Bitmap.Config;
import android.graphics.Rect;
import android.graphics.drawable.BitmapDrawable;
import android.inputmethodservice.KeyboardView;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Message;

import androidx.activity.OnBackPressedCallback;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewTreeObserver.OnGlobalLayoutListener;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.Toast;

import com.freerdp.freerdpcore.R;
import com.freerdp.freerdpcore.application.GlobalApp;
import com.freerdp.freerdpcore.application.SessionState;
import com.freerdp.freerdpcore.domain.BookmarkBase;
import com.freerdp.freerdpcore.domain.ConnectionReference;
import com.freerdp.freerdpcore.services.LibFreeRDP;
import com.freerdp.freerdpcore.utils.ClipboardManagerProxy;

import java.util.Collection;
import java.util.Iterator;

public class SessionActivity extends AppCompatActivity
    implements LibFreeRDP.UIEventListener, ClipboardManagerProxy.OnClipboardChangedListener
{
	public static final String PARAM_CONNECTION_REFERENCE = "conRef";
	public static final String PARAM_INSTANCE = "instance";
	private static final String TAG = "FreeRDP.SessionActivity";
	private Bitmap bitmap;
	private SessionState session;
	private SessionView sessionView;
	private TouchPointerView touchPointerView;
	private ProgressDialog progressDialog;

	private AlertDialog dlgVerifyCertificate;
	private AlertDialog dlgUserCredentials;
	private View userCredView;

	private UIHandler uiHandler;

	private int screen_width;
	private int screen_height;

	private boolean connectCancelledByUser = false;
	private boolean sessionRunning = false;
	private long backPressedTime = 0;

	private LibFreeRDPBroadcastReceiver libFreeRDPBroadcastReceiver;
	private ScrollView2D scrollView;
	private ClipboardManagerProxy mClipboardManager;
	private SessionInputManager inputManager;
	private boolean callbackDialogResult;

	private void createDialogs()
	{
		// build verify certificate dialog
		dlgVerifyCertificate =
		    new AlertDialog.Builder(this)
		        .setTitle(R.string.dlg_title_verify_certificate)
		        .setPositiveButton(android.R.string.yes,
		                           new DialogInterface.OnClickListener() {
			                           @Override
			                           public void onClick(DialogInterface dialog, int which)
			                           {
				                           callbackDialogResult = true;
				                           synchronized (dialog)
				                           {
					                           dialog.notify();
				                           }
			                           }
		                           })
		        .setNegativeButton(android.R.string.no,
		                           new DialogInterface.OnClickListener() {
			                           @Override
			                           public void onClick(DialogInterface dialog, int which)
			                           {
				                           callbackDialogResult = false;
				                           connectCancelledByUser = true;
				                           synchronized (dialog)
				                           {
					                           dialog.notify();
				                           }
			                           }
		                           })
		        .setCancelable(false)
		        .create();

		// build the dialog
		userCredView = getLayoutInflater().inflate(R.layout.credentials, null, true);
		dlgUserCredentials =
		    new AlertDialog.Builder(this)
		        .setView(userCredView)
		        .setTitle(R.string.dlg_title_credentials)
		        .setPositiveButton(android.R.string.ok,
		                           new DialogInterface.OnClickListener() {
			                           @Override
			                           public void onClick(DialogInterface dialog, int which)
			                           {
				                           callbackDialogResult = true;
				                           synchronized (dialog)
				                           {
					                           dialog.notify();
				                           }
			                           }
		                           })
		        .setNegativeButton(android.R.string.cancel,
		                           new DialogInterface.OnClickListener() {
			                           @Override
			                           public void onClick(DialogInterface dialog, int which)
			                           {
				                           callbackDialogResult = false;
				                           connectCancelledByUser = true;
				                           synchronized (dialog)
				                           {
					                           dialog.notify();
				                           }
			                           }
		                           })
		        .setCancelable(false)
		        .create();
	}

	private void hideSystemBars()
	{
		boolean hideStatusBar = ApplicationSettingsActivity.getHideStatusBar(this);
		boolean hideNavBar = ApplicationSettingsActivity.getHideNavigationBar(this);
		boolean hideActionBar = ApplicationSettingsActivity.getHideActionBar(this);

		// Action bar is independent of status bar and API level.
		if (getSupportActionBar() != null)
		{
			if (hideActionBar)
				getSupportActionBar().hide();
			else
				getSupportActionBar().show();
		}

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
		{
			WindowInsetsControllerCompat controller =
			    WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
			int toHide = 0;
			if (hideStatusBar)
				toHide |= WindowInsetsCompat.Type.statusBars();
			if (hideNavBar)
				toHide |= WindowInsetsCompat.Type.navigationBars();

			if (toHide != 0)
			{
				controller.hide(toHide);
				controller.setSystemBarsBehavior(
				    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
			}
			else
			{
				controller.show(WindowInsetsCompat.Type.systemBars());
			}
		}
		else
		{
			// API < 30: use deprecated setSystemUiVisibility.
			int flags = 0;
			if (hideStatusBar)
				flags |= View.SYSTEM_UI_FLAG_FULLSCREEN;
			if (hideNavBar)
				flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION;
			if (flags != 0)
				flags |= View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;

			getWindow().getDecorView().setSystemUiVisibility(flags);
		}
	}

	@Override public void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		hideSystemBars();

		this.setContentView(R.layout.session);

		Log.v(TAG, "Session.onCreate");

		// ATTENTION: We use the onGlobalLayout notification to start our
		// session.
		// This is because only then we can know the exact size of our session
		// when using fit screen
		// accounting for any status bars etc. that Android might throws on us.
		// A bit weird looking
		// but this is the only way ...
		final View activityRootView = findViewById(R.id.session_root_view);
		activityRootView.getViewTreeObserver().addOnGlobalLayoutListener(
		    new OnGlobalLayoutListener() {
			    @Override public void onGlobalLayout()
			    {
				    screen_width = activityRootView.getWidth();
				    screen_height = activityRootView.getHeight();

				    // start session
				    if (!sessionRunning && getIntent() != null)
				    {
					    processIntent(getIntent());
					    sessionRunning = true;
				    }
			    }
		    });

		sessionView = findViewById(R.id.sessionView);
		sessionView.requestFocus();

		touchPointerView = findViewById(R.id.touchPointerView);

		KeyboardView keyboardView = findViewById(R.id.extended_keyboard);
		KeyboardView modifiersKeyboardView = findViewById(R.id.extended_keyboard_header);

		scrollView = findViewById(R.id.sessionScrollView);
		uiHandler = new UIHandler();
		libFreeRDPBroadcastReceiver = new LibFreeRDPBroadcastReceiver();

		createDialogs();

		// Wire up the input manager (instance is attached later in bindSession()).
		inputManager = new SessionInputManager(this, scrollView, sessionView, touchPointerView,
		                                       keyboardView, modifiersKeyboardView);
		sessionView.setSessionViewListener(inputManager);
		touchPointerView.setTouchPointerListener(inputManager);
		sessionView.setScaleGestureDetector(
		    new ScaleGestureDetector(this, inputManager.getPinchZoomListener()));

		// register freerdp events broadcast receiver
		IntentFilter filter = new IntentFilter();
		filter.addAction(GlobalApp.ACTION_EVENT_FREERDP);
		ContextCompat.registerReceiver(this, libFreeRDPBroadcastReceiver, filter,
		                               ContextCompat.RECEIVER_EXPORTED);

		mClipboardManager = ClipboardManagerProxy.getClipboardManager(this);
		mClipboardManager.addClipboardChangedListener(this);

		getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
			@Override public void handleOnBackPressed()
			{
				handleBackPressed();
			}
		});

		hideSystemBars();
	}

	@Override public void onWindowFocusChanged(boolean hasFocus)
	{
		super.onWindowFocusChanged(hasFocus);
		if (hasFocus)
			hideSystemBars();
		mClipboardManager.getPrimaryClipManually();
	}

	@Override protected void onStart()
	{
		super.onStart();
		Log.v(TAG, "Session.onStart");
	}

	@Override protected void onRestart()
	{
		super.onRestart();
		Log.v(TAG, "Session.onRestart");
	}

	@Override protected void onResume()
	{
		super.onResume();
		Log.v(TAG, "Session.onResume");
	}

	@Override protected void onPause()
	{
		super.onPause();
		Log.v(TAG, "Session.onPause");

		// hide any visible keyboards
		inputManager.hideKeyboards();
	}

	@Override protected void onStop()
	{
		super.onStop();
		Log.v(TAG, "Session.onStop");
	}

	@Override protected void onDestroy()
	{
		if (connectThread != null)
		{
			connectThread.interrupt();
		}
		super.onDestroy();
		Log.v(TAG, "Session.onDestroy");

		// Cancel running disconnect timers.
		GlobalApp.cancelDisconnectTimer();

		// Disconnect all remaining sessions.
		Collection<SessionState> sessions = GlobalApp.getSessions();
		for (SessionState session : sessions)
			LibFreeRDP.disconnect(session.getInstance());

		// unregister freerdp events broadcast receiver
		unregisterReceiver(libFreeRDPBroadcastReceiver);

		// remove clipboard listener
		mClipboardManager.removeClipboardboardChangedListener(this);

		// free session
		GlobalApp.freeSession(session.getInstance());

		session = null;
	}

	@Override public void onConfigurationChanged(Configuration newConfig)
	{
		super.onConfigurationChanged(newConfig);

		// reload keyboard resources (changed from landscape)
		inputManager.reloadKeyboards();

		hideSystemBars();
	}

	private void processIntent(Intent intent)
	{
		// get either session instance or create one from a bookmark/uri
		Bundle bundle = intent.getExtras();
		Uri openUri = intent.getData();
		if (openUri != null)
		{
			// Launched from URI, e.g:
			// freerdp://user@ip:port/connect?sound=&rfx=&p=password&clipboard=%2b&themes=-
			connect(openUri);
		}
		else if (bundle.containsKey(PARAM_INSTANCE))
		{
			int inst = bundle.getInt(PARAM_INSTANCE);
			session = GlobalApp.getSession(inst);
			bitmap = session.getSurface().getBitmap();
			bindSession();
		}
		else if (bundle.containsKey(PARAM_CONNECTION_REFERENCE))
		{
			BookmarkBase bookmark = null;
			String refStr = bundle.getString(PARAM_CONNECTION_REFERENCE);
			if (ConnectionReference.isHostnameReference(refStr))
			{
				bookmark = new BookmarkBase();
				bookmark.setHostname(ConnectionReference.getHostname(refStr));
			}
			else if (ConnectionReference.isBookmarkReference(refStr))
			{
				bookmark = GlobalApp.getManualBookmarkGateway().findById(
				    ConnectionReference.getBookmarkId(refStr));
			}

			if (bookmark != null)
				connect(bookmark);
			else
				closeSessionActivity(RESULT_CANCELED);
		}
		else
		{
			// no session found - exit
			closeSessionActivity(RESULT_CANCELED);
		}
	}

	private void connect(BookmarkBase bookmark)
	{
		session = GlobalApp.createSession(bookmark, getApplicationContext());

		BookmarkBase.ScreenSettings screenSettings =
		    session.getBookmark().getActiveScreenSettings();
		Log.v(TAG, "Screen Resolution: " + screenSettings.getResolutionString());
		if (screenSettings.isAutomatic())
		{
			if ((getResources().getConfiguration().screenLayout &
			     Configuration.SCREENLAYOUT_SIZE_MASK) >= Configuration.SCREENLAYOUT_SIZE_LARGE)
			{
				// large screen device i.e. tablet: simply use screen info
				screenSettings.setHeight(screen_height);
				screenSettings.setWidth(screen_width);
			}
			else
			{
				// small screen device i.e. phone:
				// Automatic uses the largest side length of the screen and
				// makes a 16:10 resolution setting out of it
				int screenMax = Math.max(screen_width, screen_height);
				screenSettings.setHeight(screenMax);
				screenSettings.setWidth((int)((float)screenMax * 1.6f));
			}
		}
		if (screenSettings.isFitScreen())
		{
			screenSettings.setHeight(screen_height);
			screenSettings.setWidth(screen_width);
		}

		connectWithTitle(bookmark.getLabel());
	}

	private void connect(Uri openUri)
	{
		session = GlobalApp.createSession(openUri, getApplicationContext());

		connectWithTitle(openUri.getAuthority());
	}

	static class ConnectThread extends Thread
	{
		private final SessionState runnableSession;
		private final Context context;

		public ConnectThread(@NonNull Context context, @NonNull SessionState session)
		{
			this.context = context;
			runnableSession = session;
		}

		public void run()
		{
			runnableSession.connect(context.getApplicationContext());
		}
	}

	private ConnectThread connectThread = null;

	private void connectWithTitle(String title)
	{
		session.setUIEventListener(this);

		progressDialog = new ProgressDialog(this);
		progressDialog.setTitle(title);
		progressDialog.setMessage(getResources().getText(R.string.dlg_msg_connecting));
		progressDialog.setButton(
		    ProgressDialog.BUTTON_NEGATIVE, "Cancel", new DialogInterface.OnClickListener() {
			    @Override public void onClick(DialogInterface dialog, int which)
			    {
				    connectCancelledByUser = true;
				    LibFreeRDP.cancelConnection(session.getInstance());
			    }
		    });
		progressDialog.setCancelable(false);
		progressDialog.show();

		connectThread = new ConnectThread(getApplicationContext(), session);
		connectThread.start();
	}

	// binds the current session to the activity by wiring it up with the
	// sessionView and updating all internal objects accordingly
	private void bindSession()
	{
		Log.v(TAG, "bindSession called");
		session.setUIEventListener(this);
		sessionView.onSurfaceChange(session);
		scrollView.requestLayout();

		Bitmap surface = session.getSurface() != null ? session.getSurface().getBitmap() : null;
		inputManager.attachSession(session.getInstance(), surface);
		inputManager.setScreenSize(screen_width, screen_height);
		hideSystemBars();
	}

	private void closeSessionActivity(int resultCode)
	{
		// Go back to home activity (and send intent data back to home)
		setResult(resultCode, getIntent());
		finish();
	}

	@Override public boolean onCreateOptionsMenu(Menu menu)
	{
		getMenuInflater().inflate(R.menu.session_menu, menu);
		return true;
	}

	@Override public boolean onOptionsItemSelected(MenuItem item)
	{
		// refer to http://tools.android.com/tips/non-constant-fields why we
		// can't use switch/case here ..
		int itemId = item.getItemId();

		if (itemId == R.id.session_touch_pointer)
		{
			inputManager.toggleTouchPointer();
		}
		else if (itemId == R.id.session_sys_keyboard)
		{
			inputManager.toggleSystemKeyboard();
		}
		else if (itemId == R.id.session_ext_keyboard)
		{
			inputManager.toggleExtendedKeyboard();
		}
		else if (itemId == R.id.session_disconnect)
		{
			inputManager.hideKeyboards();
			LibFreeRDP.disconnect(session.getInstance());
		}

		return true;
	}

	public void handleBackPressed()
	{
		// hide keyboards (if any visible) or send alt+f4 to the session
		if (inputManager.isAnyKeyboardVisible())
		{
			inputManager.hideKeyboards();
			return;
		}
		if (inputManager.handleBackAsAltF4())
		{
			return;
		}
		if (System.currentTimeMillis() - backPressedTime < 2000)
		{
			LibFreeRDP.disconnect(session.getInstance());
		}
		else
		{
			backPressedTime = System.currentTimeMillis();
			Toast.makeText(this, R.string.session_double_back_to_exit, Toast.LENGTH_SHORT).show();
		}
	}

	@Override public boolean onKeyLongPress(int keyCode, KeyEvent event)
	{
		if (inputManager.onAndroidKeyLongPress(keyCode))
			return true;
		return super.onKeyLongPress(keyCode, event);
	}

	// android keyboard input handling
	// We always use the unicode value to process input from the android
	// keyboard except if key modifiers
	// (like Win, Alt, Ctrl) are activated. In this case we will send the
	// virtual key code to allow key
	// combinations (like Win + E to open the explorer).
	@Override public boolean onKeyDown(int keycode, KeyEvent event)
	{
		if (keycode == KeyEvent.KEYCODE_BACK)
			return super.onKeyDown(keycode, event);
		return inputManager.onAndroidKeyEvent(event);
	}

	@Override public boolean onKeyUp(int keycode, KeyEvent event)
	{
		if (keycode == KeyEvent.KEYCODE_BACK)
			return super.onKeyUp(keycode, event);
		return inputManager.onAndroidKeyEvent(event);
	}

	// onKeyMultiple is called for input of some special characters like umlauts
	// and some symbol characters
	@Override public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event)
	{
		return inputManager.onAndroidKeyEvent(event);
	}

	// ****************************************************************************
	// KeyboardMapper.KeyProcessingListener — delegated to SessionInputManager

	// ****************************************************************************
	// LibFreeRDP UI event listener implementation
	@Override public void OnSettingsChanged(int width, int height, int bpp)
	{

		if (bpp > 16)
			bitmap = Bitmap.createBitmap(width, height, Config.ARGB_8888);
		else
			bitmap = Bitmap.createBitmap(width, height, Config.RGB_565);

		session.setSurface(new BitmapDrawable(getResources(), bitmap));

		if (inputManager != null)
			inputManager.setBitmap(bitmap);

		if (session.getBookmark() == null)
		{
			// Return immediately if we launch from URI
			return;
		}
		// check this settings and initial settings - if they are not equal the
		// server doesn't support our settings
		// FIXME: the additional check (settings.getWidth() != width + 1) is for
		// the RDVH bug fix to avoid accidental notifications
		// (refer to android_freerdp.c for more info on this problem)
		BookmarkBase.ScreenSettings settings = session.getBookmark().getActiveScreenSettings();
		if ((settings.getWidth() != width && settings.getWidth() != width + 1) ||
		    settings.getHeight() != height || settings.getColors() != bpp)
			uiHandler.sendMessage(
			    Message.obtain(null, UIHandler.DISPLAY_TOAST,
			                   getResources().getText(R.string.info_capabilities_changed)));
	}

	@Override public void OnGraphicsUpdate(int x, int y, int width, int height)
	{
		LibFreeRDP.updateGraphics(session.getInstance(), bitmap, x, y, width, height);

		sessionView.addInvalidRegion(new Rect(x, y, x + width, y + height));

		/*
		 * since sessionView can only be modified from the UI thread any
		 * modifications to it need to be scheduled
		 */

		uiHandler.sendEmptyMessage(UIHandler.REFRESH_SESSIONVIEW);
	}

	@Override public void OnGraphicsResize(int width, int height, int bpp)
	{
		// replace bitmap
		if (bpp > 16)
			bitmap = Bitmap.createBitmap(width, height, Config.ARGB_8888);
		else
			bitmap = Bitmap.createBitmap(width, height, Config.RGB_565);
		session.setSurface(new BitmapDrawable(getResources(), bitmap));

		if (inputManager != null)
			inputManager.setBitmap(bitmap);

		/*
		 * since sessionView can only be modified from the UI thread any
		 * modifications to it need to be scheduled
		 */
		uiHandler.sendEmptyMessage(UIHandler.GRAPHICS_CHANGED);
	}

	@Override
	public boolean OnAuthenticate(StringBuilder username, StringBuilder domain,
	                              StringBuilder password)
	{
		// this is where the return code of our dialog will be stored
		callbackDialogResult = false;

		// set text fields
		((EditText)userCredView.findViewById(R.id.editTextUsername)).setText(username);
		((EditText)userCredView.findViewById(R.id.editTextDomain)).setText(domain);
		((EditText)userCredView.findViewById(R.id.editTextPassword)).setText(password);

		// start dialog in UI thread
		uiHandler.sendMessage(Message.obtain(null, UIHandler.SHOW_DIALOG, dlgUserCredentials));

		// wait for result
		try
		{
			synchronized (dlgUserCredentials)
			{
				dlgUserCredentials.wait();
			}
		}
		catch (InterruptedException e)
		{
		}

		// clear buffers
		username.setLength(0);
		domain.setLength(0);
		password.setLength(0);

		// read back user credentials
		username.append(
		    ((EditText)userCredView.findViewById(R.id.editTextUsername)).getText().toString());
		domain.append(
		    ((EditText)userCredView.findViewById(R.id.editTextDomain)).getText().toString());
		password.append(
		    ((EditText)userCredView.findViewById(R.id.editTextPassword)).getText().toString());

		return callbackDialogResult;
	}

	@Override
	public boolean OnGatewayAuthenticate(StringBuilder username, StringBuilder domain,
	                                     StringBuilder password)
	{
		// this is where the return code of our dialog will be stored
		callbackDialogResult = false;

		// set text fields
		((EditText)userCredView.findViewById(R.id.editTextUsername)).setText(username);
		((EditText)userCredView.findViewById(R.id.editTextDomain)).setText(domain);
		((EditText)userCredView.findViewById(R.id.editTextPassword)).setText(password);

		// start dialog in UI thread
		uiHandler.sendMessage(Message.obtain(null, UIHandler.SHOW_DIALOG, dlgUserCredentials));

		// wait for result
		try
		{
			synchronized (dlgUserCredentials)
			{
				dlgUserCredentials.wait();
			}
		}
		catch (InterruptedException e)
		{
		}

		// clear buffers
		username.setLength(0);
		domain.setLength(0);
		password.setLength(0);

		// read back user credentials
		username.append(
		    ((EditText)userCredView.findViewById(R.id.editTextUsername)).getText().toString());
		domain.append(
		    ((EditText)userCredView.findViewById(R.id.editTextDomain)).getText().toString());
		password.append(
		    ((EditText)userCredView.findViewById(R.id.editTextPassword)).getText().toString());

		return callbackDialogResult;
	}

	@Override
	public int OnVerifiyCertificateEx(String host, long port, String commonName, String subject,
	                                  String issuer, String fingerprint, long flags)
	{
		// see if global settings says accept all
		if (ApplicationSettingsActivity.getAcceptAllCertificates(this))
			return 0;

		// this is where the return code of our dialog will be stored
		callbackDialogResult = false;

		// set message
		String msg = getResources().getString(R.string.dlg_msg_verify_certificate);
		String type = "RDP-Server";
		if ((flags & LibFreeRDP.VERIFY_CERT_FLAG_GATEWAY) != 0)
			type = "RDP-Gateway";
		if ((flags & LibFreeRDP.VERIFY_CERT_FLAG_REDIRECT) != 0)
			type = "RDP-Redirect";
		msg += "\n\n" + type + ": " + host + ":" + port;

		msg += "\n\nSubject: " + subject + "\nIssuer: " + issuer;

		if ((flags & LibFreeRDP.VERIFY_CERT_FLAG_FP_IS_PEM) != 0)
			msg += "\nCertificate: " + fingerprint;
		else
			msg += "\nFingerprint: " + fingerprint;
		dlgVerifyCertificate.setMessage(msg);

		// start dialog in UI thread
		uiHandler.sendMessage(Message.obtain(null, UIHandler.SHOW_DIALOG, dlgVerifyCertificate));

		// wait for result
		try
		{
			synchronized (dlgVerifyCertificate)
			{
				dlgVerifyCertificate.wait();
			}
		}
		catch (InterruptedException e)
		{
		}

		return callbackDialogResult ? 1 : 0;
	}

	@Override
	public int OnVerifyChangedCertificateEx(String host, long port, String commonName,
	                                        String subject, String issuer, String fingerprint,
	                                        String oldSubject, String oldIssuer,
	                                        String oldFingerprint, long flags)
	{
		// see if global settings says accept all
		if (ApplicationSettingsActivity.getAcceptAllCertificates(this))
			return 0;

		// this is where the return code of our dialog will be stored
		callbackDialogResult = false;

		// set message
		String msg = getResources().getString(R.string.dlg_msg_verify_certificate);
		String type = "RDP-Server";
		if ((flags & LibFreeRDP.VERIFY_CERT_FLAG_GATEWAY) != 0)
			type = "RDP-Gateway";
		if ((flags & LibFreeRDP.VERIFY_CERT_FLAG_REDIRECT) != 0)
			type = "RDP-Redirect";
		msg += "\n\n" + type + ": " + host + ":" + port;
		msg += "\n\nSubject: " + subject + "\nIssuer: " + issuer;
		if ((flags & LibFreeRDP.VERIFY_CERT_FLAG_FP_IS_PEM) != 0)
			msg += "\nCertificate: " + fingerprint;
		else
			msg += "\nFingerprint: " + fingerprint;
		dlgVerifyCertificate.setMessage(msg);

		// start dialog in UI thread
		uiHandler.sendMessage(Message.obtain(null, UIHandler.SHOW_DIALOG, dlgVerifyCertificate));

		// wait for result
		try
		{
			synchronized (dlgVerifyCertificate)
			{
				dlgVerifyCertificate.wait();
			}
		}
		catch (InterruptedException e)
		{
		}

		return callbackDialogResult ? 1 : 0;
	}

	@Override public void OnRemoteClipboardChanged(String data)
	{
		Log.v(TAG, "OnRemoteClipboardChanged: " + data);
		mClipboardManager.setClipboardData(data);
	}

	// ****************************************************************************
	// SessionView.SessionViewListener and TouchPointerView.TouchPointerListener
	// — delegated to SessionInputManager

	@Override public boolean onGenericMotionEvent(MotionEvent e)
	{
		super.onGenericMotionEvent(e);
		return inputManager != null && inputManager.onGenericMotionEvent(e);
	}

	// ****************************************************************************
	// ClipboardManagerProxy.OnClipboardChangedListener
	@Override public void onClipboardChanged(String data)
	{
		Log.v(TAG, "onClipboardChanged: " + data);
		LibFreeRDP.sendClipboardData(session.getInstance(), data);
	}

	private class UIHandler extends Handler
	{

		public static final int REFRESH_SESSIONVIEW = 1;
		public static final int DISPLAY_TOAST = 2;
		public static final int SHOW_DIALOG = 5;
		public static final int GRAPHICS_CHANGED = 6;

		UIHandler()
		{
			super();
		}

		@Override public void handleMessage(Message msg)
		{
			switch (msg.what)
			{
				case GRAPHICS_CHANGED:
				{
					sessionView.onSurfaceChange(session);
					scrollView.requestLayout();
					break;
				}
				case REFRESH_SESSIONVIEW:
				{
					sessionView.invalidateRegion();
					break;
				}
				case DISPLAY_TOAST:
				{
					Toast errorToast = Toast.makeText(getApplicationContext(), msg.obj.toString(),
					                                  Toast.LENGTH_LONG);
					errorToast.show();
					break;
				}
				case SHOW_DIALOG:
				{
					// create and show the dialog
					((Dialog)msg.obj).show();
					break;
				}
			}
		}
	}

	private class LibFreeRDPBroadcastReceiver extends BroadcastReceiver
	{
		@Override public void onReceive(Context context, Intent intent)
		{
			// still got a valid session?
			if (session == null)
				return;

			// is this event for the current session?
			if (session.getInstance() != intent.getExtras().getLong(GlobalApp.EVENT_PARAM, -1))
				return;

			switch (intent.getExtras().getInt(GlobalApp.EVENT_TYPE, -1))
			{
				case GlobalApp.FREERDP_EVENT_CONNECTION_SUCCESS:
					OnConnectionSuccess(context);
					break;

				case GlobalApp.FREERDP_EVENT_CONNECTION_FAILURE:
					OnConnectionFailure(context);
					break;
				case GlobalApp.FREERDP_EVENT_DISCONNECTED:
					OnDisconnected(context);
					break;
			}
		}

		private void OnConnectionSuccess(Context context)
		{
			Log.v(TAG, "OnConnectionSuccess");

			if (connectCancelledByUser)
			{
				LibFreeRDP.disconnect(session.getInstance());
				closeSessionActivity(RESULT_CANCELED);
				return;
			}

			// bind session
			bindSession();

			if (ApplicationSettingsActivity.getKeepScreenOnWhenConnected(context))
			{
				getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
			}

			if (progressDialog != null)
			{
				progressDialog.dismiss();
				progressDialog = null;
			}

			if (session.getBookmark() == null)
			{
				// Return immediately if we launch from URI
				return;
			}

			// add hostname to history if quick connect was used
			Bundle bundle = getIntent().getExtras();
			if (bundle != null && bundle.containsKey(PARAM_CONNECTION_REFERENCE))
			{
				if (ConnectionReference.isHostnameReference(
				        bundle.getString(PARAM_CONNECTION_REFERENCE)))
				{
					assert session.getBookmark().getType() == BookmarkBase.TYPE_MANUAL;
					String item = session.getBookmark().getHostname();
					if (!GlobalApp.getQuickConnectHistoryGateway().historyItemExists(item))
						GlobalApp.getQuickConnectHistoryGateway().addHistoryItem(item);
				}
			}
		}

		private void OnConnectionFailure(Context context)
		{
			Log.v(TAG, "OnConnectionFailure");

			// cancel any pending input events
			if (inputManager != null)
				inputManager.cancelPendingEvents();

			if (progressDialog != null)
			{
				progressDialog.dismiss();
				progressDialog = null;
			}

			// post error message on UI thread
			if (!connectCancelledByUser)
				uiHandler.sendMessage(
				    Message.obtain(null, UIHandler.DISPLAY_TOAST,
				                   getResources().getText(R.string.error_connection_failure)));

			closeSessionActivity(RESULT_CANCELED);
		}

		private void OnDisconnected(Context context)
		{
			Log.v(TAG, "OnDisconnected");

			// cancel any pending input events
			if (inputManager != null)
				inputManager.cancelPendingEvents();

			if (ApplicationSettingsActivity.getKeepScreenOnWhenConnected(context))
			{
				getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
			}

			if (progressDialog != null)
			{
				progressDialog.dismiss();
				progressDialog = null;
			}

			session.setUIEventListener(null);
			closeSessionActivity(RESULT_OK);
		}
	}
}
