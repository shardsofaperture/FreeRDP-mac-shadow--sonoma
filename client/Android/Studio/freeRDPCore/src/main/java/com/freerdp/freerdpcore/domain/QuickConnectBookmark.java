/*
   Quick Connect bookmark (used for quick connects using just a hostname)

   Copyright 2013 Thincast Technologies GmbH, Author: Martin Fleisz

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
   If a copy of the MPL was not distributed with this file, You can obtain one at
   http://mozilla.org/MPL/2.0/.
*/

package com.freerdp.freerdpcore.domain;

import android.os.Parcel;
import android.os.Parcelable;

public class QuickConnectBookmark extends ManualBookmark
{

	public static final Parcelable.Creator<QuickConnectBookmark> CREATOR =
	    new Parcelable.Creator<QuickConnectBookmark>() {
		    public QuickConnectBookmark createFromParcel(Parcel in)
		    {
			    return new QuickConnectBookmark(in);
		    }

		    @Override public QuickConnectBookmark[] newArray(int size)
		    {
			    return new QuickConnectBookmark[size];
		    }
	    };

	public QuickConnectBookmark(Parcel parcel)
	{
		super(parcel);
		type = TYPE_QUICKCONNECT;
	}

	public QuickConnectBookmark()
	{
		super();
		type = TYPE_QUICKCONNECT;
	}

	private boolean directConnect = false;

	public boolean isDirectConnect()
	{
		return directConnect;
	}

	public void setDirectConnect(boolean directConnect)
	{
		this.directConnect = directConnect;
	}
}
