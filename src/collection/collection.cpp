/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <stdbool.h>

#include <QObject>
#include <QThread>
#include <QList>

#include "core/application.h"
#include "core/database.h"
#include "core/player.h"
#include "core/tagreaderclient.h"
#include "core/thread.h"
#include "core/utilities.h"
#include "core/song.h"
#include "collection.h"
#include "collectionwatcher.h"
#include "collectionbackend.h"
#include "collectionmodel.h"
#include "playlist/playlistmanager.h"

const char *SCollection::kSongsTable = "songs";
const char *SCollection::kDirsTable = "directories";
const char *SCollection::kSubdirsTable = "subdirectories";
const char *SCollection::kFtsTable = "songs_fts";

SCollection::SCollection(Application *app, QObject *parent)
    : QObject(parent),
      app_(app),
      backend_(nullptr),
      model_(nullptr),
      watcher_(nullptr),
      watcher_thread_(nullptr) {

  backend_ = new CollectionBackend();
  backend()->moveToThread(app->database()->thread());

  backend_->Init(app->database(), kSongsTable, kDirsTable, kSubdirsTable, kFtsTable);

  model_ = new CollectionModel(backend_, app_, this);

  ReloadSettings();

}

SCollection::~SCollection() {
  watcher_->Stop();
  watcher_->deleteLater();
  watcher_thread_->exit();
  watcher_thread_->wait(5000 /* five seconds */);
}

void SCollection::Init() {

  watcher_ = new CollectionWatcher(Song::Source_Collection);
  watcher_thread_ = new Thread(this);
  watcher_thread_->SetIoPriority(Utilities::IOPRIO_CLASS_IDLE);

  watcher_->moveToThread(watcher_thread_);
  watcher_thread_->start(QThread::IdlePriority);

  watcher_->set_backend(backend_);
  watcher_->set_task_manager(app_->task_manager());

  connect(backend_, SIGNAL(DirectoryDiscovered(Directory, SubdirectoryList)), watcher_, SLOT(AddDirectory(Directory, SubdirectoryList)));
  connect(backend_, SIGNAL(DirectoryDeleted(Directory)), watcher_, SLOT(RemoveDirectory(Directory)));
  connect(watcher_, SIGNAL(NewOrUpdatedSongs(SongList)), backend_, SLOT(AddOrUpdateSongs(SongList)));
  connect(watcher_, SIGNAL(SongsMTimeUpdated(SongList)), backend_, SLOT(UpdateMTimesOnly(SongList)));
  connect(watcher_, SIGNAL(SongsDeleted(SongList)), backend_, SLOT(MarkSongsUnavailable(SongList)));
  connect(watcher_, SIGNAL(SongsReadded(SongList, bool)), backend_, SLOT(MarkSongsUnavailable(SongList, bool)));
  connect(watcher_, SIGNAL(SubdirsDiscovered(SubdirectoryList)), backend_, SLOT(AddOrUpdateSubdirs(SubdirectoryList)));
  connect(watcher_, SIGNAL(SubdirsMTimeUpdated(SubdirectoryList)), backend_, SLOT(AddOrUpdateSubdirs(SubdirectoryList)));
  connect(watcher_, SIGNAL(CompilationsNeedUpdating()), backend_, SLOT(UpdateCompilations()));
  connect(app_->playlist_manager(), SIGNAL(CurrentSongChanged(Song)), SLOT(CurrentSongChanged(Song)));
  connect(app_->player(), SIGNAL(Stopped()), SLOT(Stopped()));

  // This will start the watcher checking for updates
  backend_->LoadDirectoriesAsync();
}

void SCollection::IncrementalScan() { watcher_->IncrementalScanAsync(); }

void SCollection::FullScan() { watcher_->FullScanAsync(); }

void SCollection::PauseWatcher() { watcher_->SetRescanPausedAsync(true); }

void SCollection::ResumeWatcher() { watcher_->SetRescanPausedAsync(false); }

void SCollection::ReloadSettings() {

  watcher_->ReloadSettingsAsync();

}

void SCollection::Stopped() {

  CurrentSongChanged(Song());
}

void SCollection::CurrentSongChanged(const Song &song) {  // FIXME

  TagReaderReply *reply = nullptr;

  if (reply) {
    connect(reply, SIGNAL(Finished(bool)), reply, SLOT(deleteLater()));
  }

}
