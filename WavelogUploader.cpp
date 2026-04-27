#include "WavelogUploader.h"

#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QDateTime>
#include <QTextStream>
#include <QDebug>

#include "moc_WavelogUploader.cpp"

namespace
{
  QString append_path(QString const& base, QString const& tail)
  {
    if (base.isEmpty ()) return tail;
    if (base.endsWith ('/')) return base + tail;
    return base + '/' + tail;
  }

  QString log_dir_path ()
  {
    QString dir = QStandardPaths::writableLocation (QStandardPaths::AppDataLocation);
    QDir ().mkpath (dir);
    return dir;
  }

  void append_log (QString const& line)
  {
    QString path = log_dir_path () + "/wavelog_uploader.log";
    QFile f {path};
    if (f.open (QIODevice::WriteOnly | QIODevice::Append)) {
      QTextStream ts (&f);
      ts << QDateTime::currentDateTimeUtc ().toString (Qt::ISODate) << " " << line << "\n";
    }
  }
}

WavelogUploader::Credentials WavelogUploader::read_waveloggate_config ()
{
  Credentials c;
  // Standard WaveLogGate config location (Windows). On macOS/Linux it would
  // differ but those builds aren't relevant here yet.
  QStringList candidates;
  candidates << QDir::homePath () + "/AppData/Roaming/WaveLogGate/config.json"
             << QDir::homePath () + "/AppData/Roaming/WavelogGate/config.json"
             << QDir::homePath () + "/.config/WaveLogGate/config.json";
  for (QString const& path : candidates) {
    QFile f {path};
    if (!f.open (QIODevice::ReadOnly)) continue;
    QJsonParseError err {};
    auto doc = QJsonDocument::fromJson (f.readAll (), &err);
    f.close ();
    if (err.error != QJsonParseError::NoError) continue;
    QJsonObject root = doc.object ();
    int idx = root.value ("profile").toInt (0);
    QJsonArray profiles = root.value ("profiles").toArray ();
    if (idx < 0 || idx >= profiles.size ()) idx = 0;
    if (profiles.size () == 0) continue;
    QJsonObject p = profiles.at (idx).toObject ();
    c.url = p.value ("wavelog_url").toString ();
    c.api_key = p.value ("wavelog_key").toString ();
    c.station_id = p.value ("wavelog_id").toString ();
    c.radio_name = p.value ("wavelog_radioname").toString ();
    if (c.valid ()) return c;
  }
  return c;
}

WavelogUploader::WavelogUploader (QObject * parent)
  : QObject {parent}
{}

void WavelogUploader::set_credentials (Credentials const& c)
{
  creds_ = c;
}

void WavelogUploader::upload_qso (QByteArray const& adif)
{
  if (!enabled_ || !creds_.valid () || adif.isEmpty ()) {
    if (enabled_ && !creds_.valid ()) append_log ("upload skipped: no valid credentials");
    return;
  }
  // Build JSON payload exactly like WaveLogGate.
  QJsonObject body;
  body["key"] = creds_.api_key;
  body["station_profile_id"] = creds_.station_id.isEmpty () ? "1" : creds_.station_id;
  body["type"] = "adif";
  body["string"] = QString::fromUtf8 (adif);
  QByteArray bytes = QJsonDocument {body}.toJson (QJsonDocument::Compact);

  QString endpoint = append_path (creds_.url, "index.php/api/qso");

  // Use curl via QProcess (same approach as PSKSelfMonitor for transport
  // reliability — Qt's QNAM mishandled some Cloudflare-fronted endpoints).
  // For Wavelog this is also more robust against self-signed certs etc.
  if (proc_ && proc_->state () != QProcess::NotRunning) {
    append_log ("upload skipped: previous request still in flight");
    return;
  }
  if (proc_) {
    proc_->deleteLater ();
    proc_ = nullptr;
  }
  proc_ = new QProcess (this);
  connect (proc_, QOverload<int>::of (&QProcess::finished),
           this, &WavelogUploader::on_curl_finished);
  QStringList args;
  args << "-sS"
       << "--max-time" << "30"
       << "-X" << "POST"
       << "-H" << "Content-Type: application/json"
       << "-A" << "JTDX-WavelogUploader/1.0"
       << "--data-binary" << "@-"
       << endpoint;
  // Capture the call from the ADIF for log readability
  QByteArray u = adif.toUpper ();
  int p = u.indexOf ("<CALL:");
  if (p >= 0) {
    int colon = u.indexOf ('>', p);
    if (colon > 0) {
      int len_end = u.indexOf (':', p + 6);
      if (len_end < 0 || len_end > colon) len_end = colon;
      bool ok = false;
      int len = u.mid (p + 6, len_end - (p + 6)).toInt (&ok);
      if (ok && len > 0 && colon + 1 + len <= adif.size ()) {
        pending_call_ = QString::fromUtf8 (adif.mid (colon + 1, len));
      }
    }
  }
  proc_->start ("curl", args);
  if (!proc_->waitForStarted (5000)) {
    append_log (QString ("curl failed to start: %1").arg (proc_->errorString ()));
    proc_->deleteLater ();
    proc_ = nullptr;
    emit uploaded (false, tr ("curl failed to start"));
    return;
  }
  proc_->write (bytes);
  proc_->closeWriteChannel ();
  append_log (QString ("upload posted: call=%1 endpoint=%2 bytes=%3")
              .arg (pending_call_).arg (endpoint).arg (bytes.size ()));
}

void WavelogUploader::on_curl_finished (int exit_code)
{
  if (!proc_) return;
  QProcess * p = proc_;
  proc_ = nullptr;
  QByteArray out = p->readAllStandardOutput ();
  QByteArray err = p->readAllStandardError ();
  p->deleteLater ();

  bool ok = (exit_code == 0);
  // Wavelog returns JSON; check for "status":"created" or similar.
  QJsonParseError jerr {};
  QJsonDocument d = QJsonDocument::fromJson (out, &jerr);
  QString status = "unknown";
  if (jerr.error == QJsonParseError::NoError && d.isObject ()) {
    status = d.object ().value ("status").toString ("unknown");
    if (status != "created" && status != "imported") ok = false;
  } else if (ok && out.isEmpty ()) {
    ok = false;
    status = "empty body";
  }
  append_log (QString ("upload result: call=%1 exit=%2 status=%3 body=%4 stderr=%5")
              .arg (pending_call_).arg (exit_code).arg (status)
              .arg (QString::fromUtf8 (out.left (200)).trimmed ())
              .arg (QString::fromUtf8 (err.left (120)).trimmed ()));
  emit uploaded (ok, ok ? tr ("Wavelog: %1").arg (status)
                        : tr ("Wavelog upload failed (%1): %2").arg (exit_code).arg (status));
  pending_call_.clear ();
}
