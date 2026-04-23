/*
   HomeViewModel for HomeActivity

*/

package com.freerdp.freerdpcore.presentation;

import android.app.Application;

import androidx.annotation.NonNull;
import androidx.lifecycle.AndroidViewModel;
import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;

import com.freerdp.freerdpcore.R;
import com.freerdp.freerdpcore.application.GlobalApp;
import com.freerdp.freerdpcore.domain.BookmarkBase;
import com.freerdp.freerdpcore.domain.ManualBookmark;
import com.freerdp.freerdpcore.domain.QuickConnectBookmark;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class HomeViewModel extends AndroidViewModel
{
	private final MutableLiveData<List<BookmarkBase>> bookmarks =
	    new MutableLiveData<>(Collections.emptyList());
	private String currentQuery = "";
	private final ExecutorService executor = Executors.newSingleThreadExecutor();

	public HomeViewModel(@NonNull Application application)
	{
		super(application);
	}

	public LiveData<List<BookmarkBase>> getBookmarks()
	{
		return bookmarks;
	}

	public String getCurrentQuery()
	{
		return currentQuery;
	}

	public void loadBookmarks(String query)
	{
		currentQuery = query != null ? query : "";
		executor.execute(() -> {
			List<BookmarkBase> result = new ArrayList<>();
			if (!currentQuery.isEmpty())
			{
				QuickConnectBookmark qcBm = new QuickConnectBookmark();
				qcBm.setLabel(currentQuery);
				qcBm.setHostname(currentQuery);
				qcBm.setDirectConnect(true);
				result.add(qcBm);
				result.addAll(GlobalApp.getQuickConnectHistoryGateway().findHistory(currentQuery));
				result.addAll(
				    GlobalApp.getManualBookmarkGateway().findByLabelOrHostnameLike(currentQuery));
			}
			else
			{
				result.addAll(GlobalApp.getManualBookmarkGateway().findAll());
			}
			bookmarks.postValue(result);
		});
	}

	public void deleteBookmark(long id)
	{
		executor.execute(() -> {
			GlobalApp.getManualBookmarkGateway().delete(id);
			loadBookmarks(currentQuery);
		});
	}

	@Override protected void onCleared()
	{
		super.onCleared();
		executor.shutdown();
	}
}
