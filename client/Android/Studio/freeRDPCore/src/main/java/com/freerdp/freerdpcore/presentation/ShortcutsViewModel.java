/*
   ViewModel for ShortcutsActivity

*/

package com.freerdp.freerdpcore.presentation;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import com.freerdp.freerdpcore.application.GlobalApp;
import com.freerdp.freerdpcore.domain.BookmarkBase;

import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class ShortcutsViewModel extends ViewModel
{
	private final MutableLiveData<List<BookmarkBase>> bookmarks =
	    new MutableLiveData<>(Collections.emptyList());
	private final ExecutorService executor = Executors.newSingleThreadExecutor();

	public LiveData<List<BookmarkBase>> getBookmarks()
	{
		return bookmarks;
	}

	public void loadBookmarks()
	{
		executor.execute(() -> bookmarks.postValue(GlobalApp.getManualBookmarkGateway().findAll()));
	}

	@Override protected void onCleared()
	{
		super.onCleared();
		executor.shutdown();
	}
}
