// Direct uploader to a Wavelog server, replacing the need for a separate
// WaveLogGate process. Posts QSO ADIF to /index.php/api/qso with an API key.
//
// On first run reads credentials from %APPDATA%/WaveLogGate/config.json so
// existing WaveLogGate users don't have to re-enter them, then JTDX stores
// them in its own settings.

#ifndef WAVELOG_UPLOADER_H
#define WAVELOG_UPLOADER_H

#include <QObject>
#include <QString>
#include <QByteArray>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

class WavelogUploader : public QObject
{
  Q_OBJECT
public:
  // Read credentials from WaveLogGate's config.json (active profile).
  // Returns true if a usable URL+key were found.
  struct Credentials {
    QString url;            // base URL, e.g. "http://host:8086/"
    QString api_key;
    QString station_id;     // wavelog_id
    QString radio_name;     // wavelog_radioname
    bool valid () const { return !url.isEmpty () && !api_key.isEmpty (); }
  };
  static Credentials read_waveloggate_config ();

  explicit WavelogUploader (QObject * parent = nullptr);

  void set_credentials (Credentials const&);
  void set_enabled (bool on) { enabled_ = on; }
  bool is_enabled () const { return enabled_; }
  Credentials const& credentials () const { return creds_; }

  // Submit one QSO. ADIF should already include all relevant fields.
  // Fire-and-forget; errors are logged to a rolling file.
  void upload_qso (QByteArray const& adif);

signals:
  void uploaded (bool ok, QString const& message);

private slots:
  void on_curl_finished (int exit_code);

private:
  Credentials creds_;
  bool enabled_ = false;
  QProcess * proc_ = nullptr;
  QString pending_call_;  // for log lines
};

#endif
