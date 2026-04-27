#include "PSKSelfMonitor.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QXmlStreamReader>
#include <QSet>
#include <QDebug>

#include "moc_PSKSelfMonitor.cpp"

namespace
{
  // pskreporter.info query API.
  char const * const PSKReporterQueryUrl = "https://retrieve.pskreporter.info/query";
  // Poll every 5 minutes — pskreporter.info asks for 5+ minute interval.
  int const PollIntervalMs = 5 * 60 * 1000;
  // Look-back window for spot counting.
  int const WindowMinutes = 30;
  // Coarse frequency window around dial freq to scope to the active band (Hz).
  qint64 const FreqHalfWindowHz = 100000;  // ±100 kHz
  // Don't fire repeated alerts more often than this.
  qint64 const AlertDebounceMs = 10 * 60 * 1000;
  // Identify the app to PSK Reporter operators per their docs.
  char const * const AppContact = "https://github.com/yanjz124/jtdx";
}

PSKSelfMonitor::PSKSelfMonitor (QNetworkAccessManager * network_manager, QObject * parent)
  : QObject {parent}
  , nam_ {network_manager}
  , reply_ {nullptr}
  , timer_ {new QTimer {this}}
  , dial_freq_hz_ {0}
  , enabled_ {false}
  , alert_threshold_minutes_ {15}
  , window_minutes_ {WindowMinutes}
  , last_tx_ms_ {0}
  , last_alert_ms_ {0}
{
  qRegisterMetaType<PSKSelfMonitor::Stats> ("PSKSelfMonitor::Stats");
  timer_->setInterval (PollIntervalMs);
  connect (timer_, &QTimer::timeout, this, &PSKSelfMonitor::on_timer);
}

void PSKSelfMonitor::note_tx ()
{
  qint64 now_ms = QDateTime::currentMSecsSinceEpoch ();
  last_tx_ms_ = now_ms;
  qint64 now_s = now_ms / 1000;
  // Avoid recording duplicate TXs from the same period (we get called
  // multiple times per second in some code paths).
  if (!tx_epochs_.isEmpty () && now_s - tx_epochs_.last () < 5) return;
  tx_epochs_.append (now_s);
  // Trim to the look-back window (plus a little slack).
  qint64 cutoff = now_s - qint64 (window_minutes_) * 60 - 60;
  while (!tx_epochs_.isEmpty () && tx_epochs_.first () < cutoff) tx_epochs_.removeFirst ();
}

void PSKSelfMonitor::set_target (QString const& callsign, qint64 dial_freq_hz)
{
  QString new_call = callsign.trimmed ().toUpper ();
  if (new_call != callsign_ || dial_freq_hz != dial_freq_hz_) {
    // Reset stats so we don't display stale data after a band/call change.
    last_ = Stats{};
  }
  callsign_ = new_call;
  dial_freq_hz_ = dial_freq_hz;
}

void PSKSelfMonitor::set_enabled (bool on)
{
  if (enabled_ == on) return;
  enabled_ = on;
  if (on) {
    timer_->start ();
    // Don't wait the full interval for the first poll — fire after a
    // short delay so the user sees a result quickly. Delay lets the
    // dial frequency / callsign settle if just turned on.
    QTimer::singleShot (3000, this, &PSKSelfMonitor::poll_now);
  } else {
    timer_->stop ();
  }
}

void PSKSelfMonitor::set_alert_threshold (int minutes)
{
  if (minutes < 5) minutes = 5;
  if (minutes > 120) minutes = 120;
  alert_threshold_minutes_ = minutes;
}

void PSKSelfMonitor::on_timer ()
{
  poll_now ();
}

QString PSKSelfMonitor::build_query_url () const
{
  QUrl url {PSKReporterQueryUrl};
  QUrlQuery q;
  q.addQueryItem ("senderCallsign", callsign_);
  q.addQueryItem ("flowStartSeconds", QString::number (-window_minutes_ * 60));
  q.addQueryItem ("rronly", "1");
  q.addQueryItem ("nolocator", "1");
  q.addQueryItem ("appcontact", AppContact);
  if (dial_freq_hz_ > 0) {
    qint64 lo = dial_freq_hz_ - FreqHalfWindowHz;
    qint64 hi = dial_freq_hz_ + FreqHalfWindowHz;
    if (lo < 0) lo = 0;
    q.addQueryItem ("frange", QString::number (lo) + "-" + QString::number (hi));
  }
  url.setQuery (q.query ());
  return url.toString ();
}

void PSKSelfMonitor::poll_now ()
{
  if (!enabled_ || callsign_.isEmpty () || !nam_) return;
  if (reply_) return;  // a previous request is still outstanding
  QNetworkRequest req {QUrl {build_query_url ()}};
  req.setHeader (QNetworkRequest::UserAgentHeader, "JTDX-PSKSelfMonitor/1.0");
  reply_ = nam_->get (req);
  connect (reply_, &QNetworkReply::finished, this, &PSKSelfMonitor::on_reply);
}

PSKSelfMonitor::Stats PSKSelfMonitor::parse_reply (QByteArray const& body) const
{
  Stats s;
  s.window_minutes = window_minutes_;
  s.valid = true;
  QSet<QString> calls, dxcc;
  // Collect every spot's flowStartSeconds for TX-match computation.
  QList<qint64> spot_epochs;
  QXmlStreamReader xml {body};
  while (!xml.atEnd () && !xml.hasError ()) {
    auto t = xml.readNext ();
    if (t != QXmlStreamReader::StartElement) continue;
    if (xml.name () != QStringLiteral ("receptionReport")) continue;
    auto a = xml.attributes ();
    s.spot_count++;
    QString rxc = a.value ("receiverCallsign").toString ();
    if (!rxc.isEmpty ()) calls.insert (rxc);
    QString rxdxcc = a.value ("receiverDXCCCode").toString ();
    if (!rxdxcc.isEmpty ()) {
      dxcc.insert (rxdxcc);
      s.by_dxcc[rxdxcc] = s.by_dxcc.value (rxdxcc, 0) + 1;
    }
    bool ok = false;
    int snr = a.value ("sNR").toInt (&ok);
    if (ok && snr > s.best_snr) s.best_snr = snr;
    qint64 fss = a.value ("flowStartSeconds").toLongLong (&ok);
    if (ok) {
      if (fss > s.latest_spot_epoch) s.latest_spot_epoch = fss;
      spot_epochs.append (fss);
    }
  }
  s.unique_callsigns = calls.size ();
  s.unique_dxcc = dxcc.size ();

  // Phase 2: count how many of our recent TX cycles were heard by anyone.
  // Tolerance ±60s so TR-period mismatches and decoder lag don't cause
  // false negatives; safe because TX cycles are at least 7.5s apart.
  qint64 now_s = QDateTime::currentSecsSinceEpoch ();
  qint64 cutoff = now_s - qint64 (window_minutes_) * 60;
  s.tx_count = 0;
  s.tx_heard_count = 0;
  for (qint64 tx : tx_epochs_) {
    if (tx < cutoff) continue;
    s.tx_count++;
    for (qint64 sp : spot_epochs) {
      if (qAbs (sp - tx) <= 60) { s.tx_heard_count++; break; }
    }
  }
  return s;
}

void PSKSelfMonitor::on_reply ()
{
  if (!reply_) return;
  QNetworkReply * r = reply_;
  reply_ = nullptr;

  if (r->error () != QNetworkReply::NoError) {
    QString err = r->errorString ();
    qWarning () << "PSKSelfMonitor: query failed:" << err;
    r->deleteLater ();
    // Mark stats invalid and notify UI so the label doesn't get stuck.
    Stats s;
    s.valid = false;
    s.window_minutes = window_minutes_;
    last_ = s;
    emit poll_result (last_);
    return;
  }
  QByteArray body = r->readAll ();
  r->deleteLater ();

  last_ = parse_reply (body);
  emit poll_result (last_);
  evaluate_alert (last_);
}

void PSKSelfMonitor::evaluate_alert (Stats const& stats)
{
  if (last_tx_ms_ == 0) return;  // we never TXed
  qint64 now = QDateTime::currentMSecsSinceEpoch ();
  qint64 since_tx_ms = now - last_tx_ms_;
  qint64 threshold_ms = qint64 (alert_threshold_minutes_) * 60 * 1000;
  if (since_tx_ms < threshold_ms) return;     // haven't TXed long enough yet

  // Compute time since most recent spot (if any).
  qint64 since_spot_ms = -1;
  if (stats.latest_spot_epoch > 0) {
    qint64 spot_ms = stats.latest_spot_epoch * 1000LL;
    since_spot_ms = now - spot_ms;
  }

  // Alert when:
  //   (a) no spots at all in the window AND we've been TXing recently, OR
  //   (b) the most recent spot is older than the alert threshold even
  //       though we TXed since then.
  bool alarm = false;
  if (stats.spot_count == 0) alarm = true;
  else if (since_spot_ms > threshold_ms && last_tx_ms_ > stats.latest_spot_epoch * 1000LL) alarm = true;
  if (!alarm) return;
  if (now - last_alert_ms_ < AlertDebounceMs) return;
  last_alert_ms_ = now;
  QString reason;
  if (stats.spot_count == 0) {
    reason = tr ("PSK Reporter: %1 not heard by anyone in last %2 min despite recent TX — check audio/antenna")
              .arg (callsign_).arg (window_minutes_);
  } else {
    int mins = int (since_spot_ms / 60000);
    reason = tr ("PSK Reporter: %1 last heard %2 min ago — gap since last TX, possible signal issue")
              .arg (callsign_).arg (mins);
  }
  emit no_spots_alert (reason);
}
