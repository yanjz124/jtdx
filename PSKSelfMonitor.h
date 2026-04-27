// Self-spotting watchdog that queries pskreporter.info for reception
// reports of our own callsign, alerting if we've been transmitting but
// nobody has heard us for a while (broken signal path: wrong audio
// device, antenna disconnected, fake TX, etc.).

#ifndef PSK_SELF_MONITOR_H
#define PSK_SELF_MONITOR_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QHash>
#include <QList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QProcess;

class PSKSelfMonitor : public QObject
{
  Q_OBJECT
public:
  // Aggregated stats for one poll cycle.
  struct Stats {
    int spot_count = 0;             // <receptionReport> elements
    int unique_callsigns = 0;       // distinct receiverCallsign
    int unique_dxcc = 0;            // distinct receiverDXCCCode
    int best_snr = -99;             // max sNR (dB)
    qint64 latest_spot_epoch = 0;   // max flowStartSeconds (Unix epoch)
    int window_minutes = 30;        // look-back window queried
    bool valid = false;             // false if request failed
    int tx_count = 0;               // TXs recorded in window
    int tx_heard_count = 0;         // TXs that matched at least one spot
    QHash<QString, int> by_dxcc;    // receiverDXCCCode -> spots
    QStringList receivers;          // unique receiver callsigns (for continent lookup)
    // Per-spot pairs (call, flowStartSeconds) so consumer can apply recency
    // weighting when building the continent heatmap (#5/#6).
    QList<QPair<QString, qint64>> raw_spots;
    QString error;                  // human-readable error if !valid
  };

  explicit PSKSelfMonitor (QNetworkAccessManager * network_manager, QObject * parent = nullptr);

  // Called from MainWindow when a TX cycle starts.
  void note_tx ();

  // Set the callsign and current dial frequency. JTDX recomputes the
  // band from the dial freq via Configuration::bands; we apply a coarse
  // ±100 kHz frange around the dial to filter to the active band.
  void set_target (QString const& callsign, qint64 dial_freq_hz);

  void set_enabled (bool);
  void set_alert_threshold (int minutes);

  bool is_enabled () const { return enabled_; }
  Stats const& last_stats () const { return last_; }
  qint64 last_tx_ms () const { return last_tx_ms_; }

  // Feed back receiver-by-continent counts (filled in by MainWindow after
  // its DXCC->continent lookup). Used by Phase 4 to bias CQ pick.
  void set_heard_continents (QHash<QString, int> const& by_continent);
  // True if continent had ≥1 spot in current window AND we have ≥10
  // total spots (sample-size guard).
  bool continent_known_to_hear (QString const& continent) const;

signals:
  // Emitted whenever a poll completes (even with zero spots).
  void poll_result (PSKSelfMonitor::Stats const& stats);

  // We've TXed in the last alert_threshold_ minutes but the most recent
  // spot is older than that (or there are no spots in the query window).
  void no_spots_alert (QString const& message);

public slots:
  void poll_now ();

private slots:
  void on_reply ();
  void on_curl_finished (int exit_code);
  void on_timer ();

private:
  QString build_query_url () const;
  Stats parse_reply (QByteArray const& body) const;
  void evaluate_alert (Stats const& stats);

  QNetworkAccessManager * nam_;
  QNetworkReply * reply_;
  QProcess * curl_process_;
  QTimer * timer_;
  QString callsign_;
  qint64 dial_freq_hz_;
  bool enabled_;
  int alert_threshold_minutes_;
  int window_minutes_;
  qint64 last_tx_ms_;
  qint64 last_alert_ms_;
  Stats last_;

  // Recent TX records used to compute "% TXs heard" in Phase 2.
  // Each entry is the epoch-second of one TX cycle start (matched
  // against spot flowStartSeconds with a small tolerance).
  QList<qint64> tx_epochs_;

  // Continents where we've been heard recently (Phase 4). Filled in
  // by MainWindow after DXCC→continent resolution and queried during
  // CQ selection.
  QHash<QString, int> heard_continents_;
  int total_heard_spots_ = 0;
};

Q_DECLARE_METATYPE(PSKSelfMonitor::Stats)

#endif
