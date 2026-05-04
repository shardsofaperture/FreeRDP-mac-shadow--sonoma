/* Room database for bookmark storage */

package com.freerdp.freerdpcore.data;

import android.content.Context;

import androidx.annotation.NonNull;
import androidx.room.Database;
import androidx.room.Room;
import androidx.room.RoomDatabase;
import androidx.room.migration.Migration;
import androidx.sqlite.db.SupportSQLiteDatabase;

@Database(entities = { BookmarkEntity.class }, version = AppDatabase.DB_VERSION,
          exportSchema = false)
public abstract class AppDatabase extends RoomDatabase
{
	static final int DB_VERSION = 11;
	private static final String DB_NAME = "bookmarks.db";

	private static volatile AppDatabase instance;

	public abstract BookmarkDao bookmarkDao();

	public static AppDatabase getInstance(Context context)
	{
		if (instance == null)
		{
			synchronized (AppDatabase.class)
			{
				if (instance == null)
				{
					instance = Room.databaseBuilder(context.getApplicationContext(),
					                                AppDatabase.class, DB_NAME)
					               .addMigrations(MIGRATION_10_11)
					               .build();
				}
			}
		}
		return instance;
	}

	// v10: tbl_manual_bookmarks + tbl_screen_settings + tbl_performance_flags (SQLiteOpenHelper)
	// v11: single flat `bookmarks` table (Room)
	private static final Migration MIGRATION_10_11 = new Migration(10, DB_VERSION) {
		@Override public void migrate(@NonNull SupportSQLiteDatabase db)
		{
			db.execSQL("CREATE TABLE IF NOT EXISTS `bookmarks` ("
			           + "`id` INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
			           + "`label` TEXT,"
			           + "`hostname` TEXT,"
			           + "`username` TEXT,"
			           + "`password` TEXT,"
			           + "`domain` TEXT,"
			           + "`port` INTEGER NOT NULL,"
			           + "`colors` INTEGER NOT NULL,"
			           + "`resolution` INTEGER NOT NULL,"
			           + "`width` INTEGER NOT NULL,"
			           + "`height` INTEGER NOT NULL,"
			           + "`perf_remotefx` INTEGER NOT NULL,"
			           + "`perf_gfx` INTEGER NOT NULL,"
			           + "`perf_gfx_h264` INTEGER NOT NULL,"
			           + "`perf_wallpaper` INTEGER NOT NULL,"
			           + "`perf_theming` INTEGER NOT NULL,"
			           + "`perf_full_window_drag` INTEGER NOT NULL,"
			           + "`perf_menu_animations` INTEGER NOT NULL,"
			           + "`perf_font_smoothing` INTEGER NOT NULL,"
			           + "`perf_desktop_composition` INTEGER NOT NULL,"
			           + "`screen_3g_colors` INTEGER NOT NULL,"
			           + "`screen_3g_resolution` INTEGER NOT NULL,"
			           + "`screen_3g_width` INTEGER NOT NULL,"
			           + "`screen_3g_height` INTEGER NOT NULL,"
			           + "`perf_3g_remotefx` INTEGER NOT NULL,"
			           + "`perf_3g_gfx` INTEGER NOT NULL,"
			           + "`perf_3g_gfx_h264` INTEGER NOT NULL,"
			           + "`perf_3g_wallpaper` INTEGER NOT NULL,"
			           + "`perf_3g_theming` INTEGER NOT NULL,"
			           + "`perf_3g_full_window_drag` INTEGER NOT NULL,"
			           + "`perf_3g_menu_animations` INTEGER NOT NULL,"
			           + "`perf_3g_font_smoothing` INTEGER NOT NULL,"
			           + "`perf_3g_desktop_composition` INTEGER NOT NULL,"
			           + "`enable_3g_settings` INTEGER NOT NULL,"
			           + "`enable_gateway_settings` INTEGER NOT NULL,"
			           + "`gateway_hostname` TEXT,"
			           + "`gateway_port` INTEGER NOT NULL,"
			           + "`gateway_username` TEXT,"
			           + "`gateway_password` TEXT,"
			           + "`gateway_domain` TEXT,"
			           + "`redirect_sdcard` INTEGER NOT NULL,"
			           + "`redirect_sound` INTEGER NOT NULL,"
			           + "`redirect_microphone` INTEGER NOT NULL,"
			           + "`security` INTEGER NOT NULL,"
			           + "`remote_program` TEXT,"
			           + "`work_dir` TEXT,"
			           + "`console_mode` INTEGER NOT NULL,"
			           + "`debug_level` TEXT,"
			           + "`async_channel` INTEGER NOT NULL,"
			           + "`async_update` INTEGER NOT NULL"
			           + ")");

			// port was stored as TEXT in the old schema
			db.execSQL(
			    "INSERT INTO bookmarks ("
			    + "  id, label, hostname, username, password, domain, port,"
			    + "  colors, resolution, width, height,"
			    + "  perf_remotefx, perf_gfx, perf_gfx_h264, perf_wallpaper, perf_theming,"
			    + "  perf_full_window_drag, perf_menu_animations, perf_font_smoothing, "
			    + "perf_desktop_composition,"
			    + "  screen_3g_colors, screen_3g_resolution, screen_3g_width, screen_3g_height,"
			    + "  perf_3g_remotefx, perf_3g_gfx, perf_3g_gfx_h264, perf_3g_wallpaper, "
			    + "perf_3g_theming,"
			    + "  perf_3g_full_window_drag, perf_3g_menu_animations, perf_3g_font_smoothing, "
			    + "perf_3g_desktop_composition,"
			    + "  enable_3g_settings, enable_gateway_settings,"
			    + "  gateway_hostname, gateway_port, gateway_username, gateway_password, "
			    + "gateway_domain,"
			    + "  redirect_sdcard, redirect_sound, redirect_microphone,"
			    + "  security, remote_program, work_dir, console_mode,"
			    + "  debug_level, async_channel, async_update"
			    + ") SELECT"
			    + "  b._id, IFNULL(b.label, ''), IFNULL(b.hostname, ''), IFNULL(b.username, ''), "
			    + "b.password, b.domain,"
			    + "  IFNULL(CAST(NULLIF(b.port, '') AS INTEGER), 3389),"
			    + "  IFNULL(s.colors, 32), IFNULL(s.resolution, -1), IFNULL(s.width, 0), "
			    + "IFNULL(s.height, 0),"
			    + "  IFNULL(p.perf_remotefx, 0), IFNULL(p.perf_gfx, 1), IFNULL(p.perf_gfx_h264, "
			    + "0), IFNULL(p.perf_wallpaper, 0), IFNULL(p.perf_theming, 0),"
			    + "  IFNULL(p.perf_full_window_drag, 0), IFNULL(p.perf_menu_animations, 0), "
			    + "IFNULL(p.perf_font_smoothing, 0), IFNULL(p.perf_desktop_composition, 0),"
			    + "  IFNULL(s3.colors, 16), IFNULL(s3.resolution, -1), IFNULL(s3.width, 0), "
			    + "IFNULL(s3.height, 0),"
			    + "  IFNULL(p3.perf_remotefx, 0), IFNULL(p3.perf_gfx, 0), IFNULL(p3.perf_gfx_h264, "
			    + "0), IFNULL(p3.perf_wallpaper, 0), IFNULL(p3.perf_theming, 0),"
			    + "  IFNULL(p3.perf_full_window_drag, 0), IFNULL(p3.perf_menu_animations, 0), "
			    + "IFNULL(p3.perf_font_smoothing, 0), IFNULL(p3.perf_desktop_composition, 0),"
			    + "  IFNULL(b.enable_3g_settings, 0), IFNULL(b.enable_gateway_settings, 0),"
			    + "  b.gateway_hostname, IFNULL(b.gateway_port, 443), b.gateway_username, "
			    + "b.gateway_password, b.gateway_domain,"
			    + "  IFNULL(b.redirect_sdcard, 0), IFNULL(b.redirect_sound, 0), "
			    + "IFNULL(b.redirect_microphone, 0),"
			    +
			    "  IFNULL(b.security, 0), b.remote_program, b.work_dir, IFNULL(b.console_mode, 0),"
			    + "  IFNULL(b.debug_level, 'INFO'), IFNULL(b.async_channel, 0), "
			    + "IFNULL(b.async_update, 0)"
			    + " FROM tbl_manual_bookmarks b"
			    + " LEFT JOIN tbl_screen_settings s  ON s._id  = b.screen_settings"
			    + " LEFT JOIN tbl_screen_settings s3 ON s3._id = b.screen_3g"
			    + " LEFT JOIN tbl_performance_flags p  ON p._id  = b.performance_flags"
			    + " LEFT JOIN tbl_performance_flags p3 ON p3._id = b.performance_3g");

			db.execSQL("DROP TABLE IF EXISTS tbl_manual_bookmarks");
			db.execSQL("DROP TABLE IF EXISTS tbl_screen_settings");
			db.execSQL("DROP TABLE IF EXISTS tbl_performance_flags");
		}
	};
}
