/*
   SessionViewModel — exposes RDP connection state as LiveData

   Copyright 2026 Thincast Technologies GmbH

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
   If a copy of the MPL was not distributed with this file, You can obtain one at
   http://mozilla.org/MPL/2.0/.
*/

package com.freerdp.freerdpcore.presentation;

import android.app.Application;

import androidx.annotation.NonNull;
import androidx.lifecycle.AndroidViewModel;
import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;

import com.freerdp.freerdpcore.application.GlobalApp;

public class SessionViewModel extends AndroidViewModel
{
	public enum ConnectionState
	{
		IDLE,
		CONNECTING,
		CONNECTED,
		FAILED,
		DISCONNECTED
	}

	private final MutableLiveData<ConnectionState> state = new MutableLiveData<>(ConnectionState.IDLE);
	private long registeredInstance = 0;

	public SessionViewModel(@NonNull Application application)
	{
		super(application);
	}

	public LiveData<ConnectionState> getState()
	{
		return state;
	}

	public void register(long instance)
	{
		unregister();
		registeredInstance = instance;
		state.setValue(ConnectionState.CONNECTING);
		GlobalApp.registerSessionListener(instance, new GlobalApp.SessionEventListener() {
			@Override public void onConnectionSuccess()
			{
				state.setValue(ConnectionState.CONNECTED);
			}

			@Override public void onConnectionFailure()
			{
				state.setValue(ConnectionState.FAILED);
			}

			@Override public void onDisconnected()
			{
				state.setValue(ConnectionState.DISCONNECTED);
			}
		});
	}

	public void unregister()
	{
		if (registeredInstance != 0)
		{
			GlobalApp.unregisterSessionListener(registeredInstance);
			registeredInstance = 0;
		}
	}

	@Override protected void onCleared()
	{
		unregister();
		super.onCleared();
	}
}
