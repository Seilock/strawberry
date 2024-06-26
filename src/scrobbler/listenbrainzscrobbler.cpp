/*
 * Strawberry Music Player
 * Copyright 2018-2023, Jonas Kvinge <jonas@jkvinge.net>
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

#include <algorithm>

#include <QCoreApplication>
#include <QtGlobal>
#include <QDesktopServices>
#include <QVariant>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QTimer>
#include <QMessageBox>
#include <QSettings>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include "core/shared_ptr.h"
#include "core/networkaccessmanager.h"
#include "core/song.h"
#include "core/logging.h"
#include "core/settings.h"
#include "utilities/timeconstants.h"
#include "internet/localredirectserver.h"
#include "settings/scrobblersettingspage.h"

#include "scrobblersettings.h"
#include "scrobblerservice.h"
#include "scrobblercache.h"
#include "scrobblercacheitem.h"
#include "scrobblemetadata.h"
#include "listenbrainzscrobbler.h"

const char *ListenBrainzScrobbler::kName = "ListenBrainz";
const char *ListenBrainzScrobbler::kSettingsGroup = "ListenBrainz";

namespace {
constexpr char kOAuthAuthorizeUrl[] = "https://musicbrainz.org/oauth2/authorize";
constexpr char kOAuthAccessTokenUrl[] = "https://musicbrainz.org/oauth2/token";
constexpr char kOAuthRedirectUrl[] = "http://localhost";
constexpr char kApiUrl[] = "https://api.listenbrainz.org";
constexpr char kClientIDB64[] = "b2VBVU53cVNRZXIwZXIwOUZpcWkwUQ==";
constexpr char kClientSecretB64[] = "Uk9GZ2hrZVEzRjNvUHlFaHFpeVdQQQ==";
constexpr char kCacheFile[] = "listenbrainzscrobbler.cache";
constexpr int kScrobblesPerRequest = 10;
}  // namespace

ListenBrainzScrobbler::ListenBrainzScrobbler(SharedPtr<ScrobblerSettings> settings, SharedPtr<NetworkAccessManager> network, QObject *parent)
    : ScrobblerService(QLatin1String(kName), settings, parent),
      network_(network),
      cache_(new ScrobblerCache(QLatin1String(kCacheFile), this)),
      server_(nullptr),
      enabled_(false),
      expires_in_(-1),
      login_time_(0),
      submitted_(false),
      scrobbled_(false),
      timestamp_(0),
      submit_error_(false),
      prefer_albumartist_(false) {

  refresh_login_timer_.setSingleShot(true);
  QObject::connect(&refresh_login_timer_, &QTimer::timeout, this, &ListenBrainzScrobbler::RequestNewAccessToken);

  timer_submit_.setSingleShot(true);
  QObject::connect(&timer_submit_, &QTimer::timeout, this, &ListenBrainzScrobbler::Submit);

  ListenBrainzScrobbler::ReloadSettings();
  LoadSession();

}

ListenBrainzScrobbler::~ListenBrainzScrobbler() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }

  if (server_) {
    QObject::disconnect(server_, nullptr, this, nullptr);
    if (server_->isListening()) server_->close();
    server_->deleteLater();
  }

}

void ListenBrainzScrobbler::ReloadSettings() {

  Settings s;
  s.beginGroup(kSettingsGroup);
  enabled_ = s.value("enabled", false).toBool();
  user_token_ = s.value("user_token").toString();
  s.endGroup();

  s.beginGroup(ScrobblerSettingsPage::kSettingsGroup);
  prefer_albumartist_ = s.value("albumartist", false).toBool();
  s.endGroup();

}

void ListenBrainzScrobbler::LoadSession() {

  Settings s;
  s.beginGroup(kSettingsGroup);
  access_token_ = s.value("access_token").toString();
  expires_in_ = s.value("expires_in", -1).toInt();
  token_type_ = s.value("token_type").toString();
  refresh_token_ = s.value("refresh_token").toString();
  login_time_ = s.value("login_time").toLongLong();
  s.endGroup();

  if (!refresh_token_.isEmpty()) {
    qint64 time = expires_in_ - (QDateTime::currentDateTime().toSecsSinceEpoch() - static_cast<qint64>(login_time_));
    if (time < 6) time = 6;
    refresh_login_timer_.setInterval(static_cast<int>(time * kMsecPerSec));
    refresh_login_timer_.start();
  }

}

void ListenBrainzScrobbler::Logout() {

  access_token_.clear();
  token_type_.clear();
  refresh_token_.clear();
  expires_in_ = -1;
  login_time_ = 0;

  Settings s;
  s.beginGroup(kSettingsGroup);
  s.remove("access_token");
  s.remove("expires_in");
  s.remove("token_type");
  s.remove("refresh_token");
  s.endGroup();

}

void ListenBrainzScrobbler::Authenticate() {

  if (!server_) {
    server_ = new LocalRedirectServer(this);
    if (!server_->Listen()) {
      AuthError(server_->error());
      delete server_;
      server_ = nullptr;
      return;
    }
    QObject::connect(server_, &LocalRedirectServer::Finished, this, &ListenBrainzScrobbler::RedirectArrived);
  }

  QUrl redirect_url(QString::fromLatin1(kOAuthRedirectUrl));
  redirect_url.setPort(server_->url().port());

  QUrlQuery url_query;
  url_query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
  url_query.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(QByteArray::fromBase64(kClientIDB64)));
  url_query.addQueryItem(QStringLiteral("redirect_uri"), redirect_url.toString());
  url_query.addQueryItem(QStringLiteral("scope"), QStringLiteral("profile;email;tag;rating;collection;submit_isrc;submit_barcode"));
  QUrl url(QString::fromLatin1(kOAuthAuthorizeUrl));
  url.setQuery(url_query);

  bool result = QDesktopServices::openUrl(url);
  if (!result) {
    QMessageBox messagebox(QMessageBox::Information, tr("ListenBrainz Authentication"), tr("Please open this URL in your browser") + QStringLiteral(":<br /><a href=\"%1\">%1</a>").arg(url.toString()), QMessageBox::Ok);
    messagebox.setTextFormat(Qt::RichText);
    messagebox.exec();
  }

}

void ListenBrainzScrobbler::RedirectArrived() {

  if (!server_) return;

  if (server_->error().isEmpty()) {
    QUrl url = server_->request_url();
    if (url.isValid()) {
      QUrlQuery url_query(url);
      if (url_query.hasQueryItem(QStringLiteral("error"))) {
        AuthError(QUrlQuery(url).queryItemValue(QStringLiteral("error")));
      }
      else if (url_query.hasQueryItem(QStringLiteral("code"))) {
        RequestAccessToken(url, url_query.queryItemValue(QStringLiteral("code")));
      }
      else {
        AuthError(tr("Redirect missing token code!"));
      }
    }
    else {
      AuthError(tr("Received invalid reply from web browser."));
    }
  }
  else {
    AuthError(server_->error());
  }

  server_->close();
  server_->deleteLater();
  server_ = nullptr;

}

ListenBrainzScrobbler::ReplyResult ListenBrainzScrobbler::GetJsonObject(QNetworkReply *reply, QJsonObject &json_obj, QString &error_description) {

  ReplyResult reply_error_type = ReplyResult::ServerError;

  if (reply->error() == QNetworkReply::NoError) {
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
      reply_error_type = ReplyResult::Success;
    }
    else {
      error_description = QStringLiteral("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    }
  }
  else {
    error_description = QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(reply->error());
  }

  // See if there is Json data containing "error" and "error_description" or "code" and "error" - then use that instead.
  if (reply->error() == QNetworkReply::NoError || reply->error() >= 200) {
    const QByteArray data = reply->readAll();
    if (!data.isEmpty() && ExtractJsonObj(data, json_obj, error_description)) {
      if (json_obj.contains(QStringLiteral("error")) && json_obj.contains(QStringLiteral("error_description"))) {
        error_description = json_obj[QStringLiteral("error_description")].toString();
        reply_error_type = ReplyResult::APIError;
      }
      else if (json_obj.contains(QStringLiteral("code")) && json_obj.contains(QStringLiteral("error"))) {
        error_description = QStringLiteral("%1 (%2)").arg(json_obj[QStringLiteral("error")].toString()).arg(json_obj[QStringLiteral("code")].toInt());
        reply_error_type = ReplyResult::APIError;
      }
    }
    if (reply->error() == QNetworkReply::ContentAccessDenied || reply->error() == QNetworkReply::ContentOperationNotPermittedError || reply->error() == QNetworkReply::AuthenticationRequiredError) {
      // Session is probably expired
      Logout();
    }
  }

  return reply_error_type;

}

void ListenBrainzScrobbler::RequestAccessToken(const QUrl &redirect_url, const QString &code) {

  refresh_login_timer_.stop();

  ParamList params = ParamList() << Param(QStringLiteral("client_id"), QString::fromLatin1(QByteArray::fromBase64(kClientIDB64)))
                                 << Param(QStringLiteral("client_secret"), QString::fromLatin1(QByteArray::fromBase64(kClientSecretB64)));

  if (!code.isEmpty() && !redirect_url.isEmpty()) {
    params << Param(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    params << Param(QStringLiteral("code"), code);
    params << Param(QStringLiteral("redirect_uri"), redirect_url.toString());
  }
  else if (!refresh_token_.isEmpty() && enabled_) {
    params << Param(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    params << Param(QStringLiteral("refresh_token"), refresh_token_);
  }
  else {
    return;
  }

  QUrlQuery url_query;
  for (const Param &param : params) {
    url_query.addQueryItem(QString::fromLatin1(QUrl::toPercentEncoding(param.first)), QString::fromLatin1(QUrl::toPercentEncoding(param.second)));
  }

  QUrl session_url(QString::fromLatin1(kOAuthAccessTokenUrl));

  QNetworkRequest req(session_url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
  QByteArray query = url_query.toString(QUrl::FullyEncoded).toUtf8();
  QNetworkReply *reply = network_->post(req, query);
  replies_ << reply;
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { AuthenticateReplyFinished(reply); });

}

void ListenBrainzScrobbler::AuthenticateReplyFinished(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    AuthError(error_message);
    return;
  }

  if (!json_obj.contains(QStringLiteral("access_token")) || !json_obj.contains(QStringLiteral("expires_in")) || !json_obj.contains(QStringLiteral("token_type"))) {
    AuthError(QStringLiteral("Json access_token, expires_in or token_type is missing."));
    return;
  }

  access_token_ = json_obj[QStringLiteral("access_token")].toString();
  expires_in_ = json_obj[QStringLiteral("expires_in")].toInt();
  token_type_ = json_obj[QStringLiteral("token_type")].toString();
  if (json_obj.contains(QStringLiteral("refresh_token"))) {
    refresh_token_ = json_obj[QStringLiteral("refresh_token")].toString();
  }
  login_time_ = QDateTime::currentDateTime().toSecsSinceEpoch();

  Settings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("access_token", access_token_);
  s.setValue("expires_in", expires_in_);
  s.setValue("token_type", token_type_);
  s.setValue("refresh_token", refresh_token_);
  s.setValue("login_time", login_time_);
  s.endGroup();

  if (expires_in_ > 0) {
    refresh_login_timer_.setInterval(static_cast<int>(expires_in_ * kMsecPerSec));
    refresh_login_timer_.start();
  }

  emit AuthenticationComplete(true);

  qLog(Debug) << "ListenBrainz: Authentication was successful, login expires in" << expires_in_;

  StartSubmit();

}

QNetworkReply *ListenBrainzScrobbler::CreateRequest(const QUrl &url, const QJsonDocument &json_doc) {

  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  req.setRawHeader("Authorization", QStringLiteral("Token %1").arg(user_token_).toUtf8());
  QNetworkReply *reply = network_->post(req, json_doc.toJson());
  replies_ << reply;

  //qLog(Debug) << "ListenBrainz: Sending request" << json_doc.toJson();

  return reply;

}

QJsonObject ListenBrainzScrobbler::JsonTrackMetadata(const ScrobbleMetadata &metadata) const {

  QJsonObject object_track_metadata;
  if (prefer_albumartist_) {
    object_track_metadata.insert(QStringLiteral("artist_name"), QJsonValue::fromVariant(metadata.effective_albumartist()));
  }
  else {
    object_track_metadata.insert(QStringLiteral("artist_name"), QJsonValue::fromVariant(metadata.artist));
  }

  if (!metadata.album.isEmpty()) {
    object_track_metadata.insert(QStringLiteral("release_name"), QJsonValue::fromVariant(StripAlbum(metadata.album)));
  }

  object_track_metadata.insert(QStringLiteral("track_name"), QJsonValue::fromVariant(StripTitle(metadata.title)));

  QJsonObject object_additional_info;

  if (metadata.length_nanosec > 0) {
    object_additional_info.insert(QStringLiteral("duration_ms"), metadata.length_nanosec / kNsecPerMsec);
  }

  if (metadata.track > 0) {
    object_additional_info.insert(QStringLiteral("tracknumber"), metadata.track);
  }

  object_additional_info.insert(QStringLiteral("media_player"), QCoreApplication::applicationName());
  object_additional_info.insert(QStringLiteral("media_player_version"), QCoreApplication::applicationVersion());
  object_additional_info.insert(QStringLiteral("submission_client"), QCoreApplication::applicationName());
  object_additional_info.insert(QStringLiteral("submission_client_version"), QCoreApplication::applicationVersion());

  QStringList artist_mbids_list;
  if (!metadata.musicbrainz_album_artist_id.isEmpty()) {
    artist_mbids_list << metadata.musicbrainz_album_artist_id.split(QLatin1Char('/'));
  }
  if (!metadata.musicbrainz_artist_id.isEmpty()) {
    artist_mbids_list << metadata.musicbrainz_artist_id.split(QLatin1Char('/'));
  }
  if (!metadata.musicbrainz_original_artist_id.isEmpty()) {
    artist_mbids_list << metadata.musicbrainz_original_artist_id.split(QLatin1Char('/'));
  }
  if (!artist_mbids_list.isEmpty()) {
    QJsonArray artist_mbids_array;
    for (const QString &musicbrainz_artist_id : artist_mbids_list) {
      if (!musicbrainz_artist_id.isEmpty() && !artist_mbids_array.contains(musicbrainz_artist_id)) {
        artist_mbids_array.append(musicbrainz_artist_id);
      }
    }
    if (!artist_mbids_array.isEmpty()) {
      object_additional_info.insert(QStringLiteral("artist_mbids"), artist_mbids_array);
    }
  }

  if (!metadata.musicbrainz_album_id.isEmpty()) {
    object_additional_info.insert(QStringLiteral("release_mbid"), metadata.musicbrainz_album_id);
  }
  else if (!metadata.musicbrainz_original_album_id.isEmpty()) {
    object_additional_info.insert(QStringLiteral("release_mbid"), metadata.musicbrainz_original_album_id);
  }

  if (!metadata.musicbrainz_recording_id.isEmpty()) {
    object_additional_info.insert(QStringLiteral("recording_mbid"), metadata.musicbrainz_recording_id);
  }
  if (!metadata.musicbrainz_track_id.isEmpty()) {
    object_additional_info.insert(QStringLiteral("track_mbid"), metadata.musicbrainz_track_id);
  }
  if (!metadata.musicbrainz_work_id.isEmpty()) {
    object_additional_info.insert(QStringLiteral("work_mbids"), QJsonArray() << metadata.musicbrainz_work_id);
  }

  object_track_metadata.insert(QStringLiteral("additional_info"), object_additional_info);

  return object_track_metadata;

}

void ListenBrainzScrobbler::UpdateNowPlaying(const Song &song) {

  CheckScrobblePrevSong();

  song_playing_ = song;
  scrobbled_ = false;
  timestamp_ = QDateTime::currentDateTime().toSecsSinceEpoch();

  if (!song.is_metadata_good() || !authenticated() || settings_->offline()) return;

  QJsonObject object_listen;
  object_listen.insert(QStringLiteral("track_metadata"), JsonTrackMetadata(ScrobbleMetadata(song)));
  QJsonArray array_payload;
  array_payload.append(object_listen);
  QJsonObject object;
  object.insert(QStringLiteral("listen_type"), QStringLiteral("playing_now"));
  object.insert(QStringLiteral("payload"), array_payload);
  QJsonDocument doc(object);

  QUrl url(QStringLiteral("%1/1/submit-listens").arg(QLatin1String(kApiUrl)));
  QNetworkReply *reply = CreateRequest(url, doc);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { UpdateNowPlayingRequestFinished(reply); });

}

void ListenBrainzScrobbler::UpdateNowPlayingRequestFinished(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_description;
  if (GetJsonObject(reply, json_obj, error_description) != ReplyResult::Success) {
    Error(error_description);
    return;
  }

  if (!json_obj.contains(QStringLiteral("status"))) {
    Error(QStringLiteral("Now playing request is missing status from server."));
    return;
  }

  QString status = json_obj[QStringLiteral("status")].toString();
  if (status.compare(QLatin1String("ok"), Qt::CaseInsensitive) != 0) {
    Error(QStringLiteral("Received %1 status for now playing.").arg(status));
  }

}

void ListenBrainzScrobbler::ClearPlaying() {

  CheckScrobblePrevSong();
  song_playing_ = Song();
  scrobbled_ = false;
  timestamp_ = 0;

}

void ListenBrainzScrobbler::Scrobble(const Song &song) {

  if (song.id() != song_playing_.id() || song.url() != song_playing_.url() || !song.is_metadata_good()) return;

  scrobbled_ = true;

  cache_->Add(song, timestamp_);

  if (settings_->offline() || !authenticated()) return;

  StartSubmit();

}

void ListenBrainzScrobbler::StartSubmit(const bool initial) {

  if (!submitted_ && cache_->Count() > 0) {
    if (initial && settings_->submit_delay() <= 0 && !submit_error_) {
      if (timer_submit_.isActive()) {
        timer_submit_.stop();
      }
      Submit();
    }
    else if (!timer_submit_.isActive()) {
      int submit_delay = static_cast<int>(std::max(settings_->submit_delay(), submit_error_ ? 30 : 5) * kMsecPerSec);
      timer_submit_.setInterval(submit_delay);
      timer_submit_.start();
    }
  }

}

void ListenBrainzScrobbler::Submit() {

  qLog(Debug) << "ListenBrainz: Submitting scrobbles.";

  if (!enabled() || !authenticated() || settings_->offline()) return;

  QJsonArray array;
  ScrobblerCacheItemPtrList cache_items_sent;
  ScrobblerCacheItemPtrList all_cache_items = cache_->List();
  for (ScrobblerCacheItemPtr cache_item : all_cache_items) {
    if (cache_item->sent) continue;
    if (cache_item->error && cache_items_sent.count() > 0) break;
    cache_item->sent = true;
    cache_items_sent << cache_item;
    QJsonObject object_listen;
    object_listen.insert(QStringLiteral("listened_at"), QJsonValue::fromVariant(cache_item->timestamp));
    object_listen.insert(QStringLiteral("track_metadata"), JsonTrackMetadata(cache_item->metadata));
    array.append(QJsonValue::fromVariant(object_listen));
    if (cache_items_sent.count() >= kScrobblesPerRequest || cache_item->error) break;
  }

  if (cache_items_sent.count() <= 0) return;

  submitted_ = true;

  QJsonObject object;
  object.insert(QStringLiteral("listen_type"), QStringLiteral("import"));
  object.insert(QStringLiteral("payload"), array);
  QJsonDocument doc(object);

  QUrl url(QStringLiteral("%1/1/submit-listens").arg(QLatin1String(kApiUrl)));
  QNetworkReply *reply = CreateRequest(url, doc);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, cache_items_sent]() { ScrobbleRequestFinished(reply, cache_items_sent); });

}

void ListenBrainzScrobbler::ScrobbleRequestFinished(QNetworkReply *reply, ScrobblerCacheItemPtrList cache_items) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  submitted_ = false;

  QJsonObject json_obj;
  QString error_message;
  const ReplyResult reply_result = GetJsonObject(reply, json_obj, error_message);
  if (reply_result == ReplyResult::Success) {
    if (json_obj.contains(QStringLiteral("status"))) {
      QString status = json_obj[QStringLiteral("status")].toString();
      qLog(Debug) << "ListenBrainz: Received scrobble status:" << status;
    }
    else {
      qLog(Debug) << "ListenBrainz: Received scrobble reply without status.";
    }
    cache_->Flush(cache_items);
    submit_error_ = false;
  }
  else {
    submit_error_ = true;
    if (reply_result == ReplyResult::APIError) {
      if (cache_items.count() == 1) {
        const ScrobbleMetadata &metadata = cache_items.first()->metadata;
        Error(tr("Unable to scrobble %1 - %2 because of error: %3").arg(metadata.effective_albumartist()).arg(metadata.title).arg(error_message));
        cache_->Flush(cache_items);
      }
      else {
        Error(error_message);
        cache_->SetError(cache_items);
        cache_->ClearSent(cache_items);
      }
    }
    else {
      Error(error_message);
      cache_->ClearSent(cache_items);
    }
  }

  StartSubmit();

}

void ListenBrainzScrobbler::Love() {

  if (!song_playing_.is_valid() || !song_playing_.is_metadata_good()) return;

  if (!authenticated()) settings_->ShowConfig();

  if (song_playing_.musicbrainz_recording_id().isEmpty()) {
    Error(tr("Missing MusicBrainz recording ID for %1 %2 %3").arg(song_playing_.artist()).arg(song_playing_.album()).arg(song_playing_.title()));
    return;
  }

  qLog(Debug) << "ListenBrainz: Sending love for song" << song_playing_.artist() << song_playing_.album() << song_playing_.title();

  QJsonObject object;
  object.insert(QStringLiteral("recording_mbid"), song_playing_.musicbrainz_recording_id());
  object.insert(QStringLiteral("score"), 1);

  QUrl url(QStringLiteral("%1/1/feedback/recording-feedback").arg(QLatin1String(kApiUrl)));
  QNetworkReply *reply = CreateRequest(url, QJsonDocument(object));
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { LoveRequestFinished(reply); });

}

void ListenBrainzScrobbler::LoveRequestFinished(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    Error(error_message);
    return;
  }

  if (json_obj.contains(QStringLiteral("status"))) {
    qLog(Debug) << "ListenBrainz: Received recording-feedback status:" << json_obj[QStringLiteral("status")].toString();
  }

}

void ListenBrainzScrobbler::AuthError(const QString &error) {

  qLog(Error) << "ListenBrainz" << error;
  emit AuthenticationComplete(false, error);

}

void ListenBrainzScrobbler::Error(const QString &error, const QVariant &debug) {

  qLog(Error) << "ListenBrainz:" << error;
  if (debug.isValid()) qLog(Debug) << debug;

  if (settings_->show_error_dialog()) {
    emit ErrorMessage(tr("ListenBrainz error: %1").arg(error));
  }

}

void ListenBrainzScrobbler::CheckScrobblePrevSong() {

  qint64 duration = QDateTime::currentDateTime().toSecsSinceEpoch() - static_cast<qint64>(timestamp_);
  if (duration < 0) duration = 0;

  if (!scrobbled_ && song_playing_.is_metadata_good() && song_playing_.is_radio() && duration > 30) {
    Song song(song_playing_);
    song.set_length_nanosec(duration * kNsecPerSec);
    Scrobble(song);
  }

}
