/*
   Manual bookmarks database gateway

   Copyright 2013 Thincast Technologies GmbH, Author: Martin Fleisz

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
   If a copy of the MPL was not distributed with this file, You can obtain one at
   http://mozilla.org/MPL/2.0/.
*/

package com.freerdp.freerdpcore.services;

import androidx.lifecycle.LiveData;

import com.freerdp.freerdpcore.data.BookmarkConverter;
import com.freerdp.freerdpcore.data.BookmarkDao;
import com.freerdp.freerdpcore.data.BookmarkEntity;
import com.freerdp.freerdpcore.domain.BookmarkBase;
import com.freerdp.freerdpcore.domain.ManualBookmark;

import java.util.ArrayList;
import java.util.List;

public class ManualBookmarkGateway
{
	private final BookmarkDao dao;

	public ManualBookmarkGateway(BookmarkDao dao)
	{
		this.dao = dao;
	}

	public LiveData<List<BookmarkEntity>> getAllLiveData()
	{
		return dao.getAllLiveData();
	}

	public ArrayList<BookmarkBase> findAll()
	{
		List<BookmarkEntity> entities = dao.getAll();
		ArrayList<BookmarkBase> result = new ArrayList<>(entities.size());
		for (BookmarkEntity e : entities)
			result.add(BookmarkConverter.toManualBookmark(e));
		return result;
	}

	public BookmarkBase findById(long id)
	{
		BookmarkEntity e = dao.getById(id);
		return (e != null) ? BookmarkConverter.toManualBookmark(e) : null;
	}

	public long insert(BookmarkBase bookmark)
	{
		long newId = dao.insert(BookmarkConverter.toEntity((ManualBookmark)bookmark));
		bookmark.setId(newId);
		return newId;
	}

	public boolean update(BookmarkBase bookmark)
	{
		dao.update(BookmarkConverter.toEntity((ManualBookmark)bookmark));
		return true;
	}

	public boolean delete(long id)
	{
		dao.deleteById(id);
		return true;
	}

	public ArrayList<BookmarkBase> findByLabelOrHostnameLike(String pattern)
	{
		List<BookmarkEntity> entities = dao.search("%" + pattern + "%");
		ArrayList<BookmarkBase> result = new ArrayList<>(entities.size());
		for (BookmarkEntity e : entities)
			result.add(BookmarkConverter.toManualBookmark(e));
		return result;
	}
}
