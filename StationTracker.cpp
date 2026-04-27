#include "StationTracker.h"

#include <QDateTime>

#include "Radio.hpp"

StationTracker::Behavior StationTracker::empty_;

bool StationTracker::Behavior::is_cqing_now () const
{
  if (last_heard_ms == 0) return false;
  qint64 now = QDateTime::currentMSecsSinceEpoch ();
  if (now - last_heard_ms > 30000) return false;
  // The last decode was a CQ if their currently-working slot is empty.
  return currently_in_qso_with.isEmpty ();
}

int StationTracker::Behavior::avg_snr () const
{
  if (total_decoded == 0) return -99;
  return sum_snr / total_decoded;
}

double StationTracker::Behavior::reply_rate () const
{
  if (times_we_called == 0) return -1.0;  // unknown
  return double (times_replied_to_us) / double (times_we_called);
}

void StationTracker::note_decode (QString const& sender_call, QString const& target_call,
                                  QString const& my_base_call, bool is_cq, int snr,
                                  QString const& grid_hint, qint64 now_ms)
{
  if (sender_call.isEmpty ()) return;
  QString base = Radio::base_callsign (sender_call);
  if (base.length () < 3) return;
  Behavior& b = data_[base];
  if (b.first_heard_ms == 0) b.first_heard_ms = now_ms;
  b.last_heard_ms = now_ms;
  b.total_decoded++;
  if (snr > -90 && snr < 60) {
    b.sum_snr += snr;
    if (snr > b.best_snr) b.best_snr = snr;
  }
  if (!grid_hint.isEmpty () && grid_hint.length () >= 4) b.grid = grid_hint;

  QString my_base = my_base_call.isEmpty () ? QString () : Radio::base_callsign (my_base_call);
  bool target_is_us = (!my_base.isEmpty () && Radio::base_callsign (target_call) == my_base);

  if (is_cq) {
    b.times_cqing++;
    b.currently_in_qso_with.clear ();
    b.currently_in_qso_since_ms = 0;
  } else if (target_is_us) {
    b.times_replied_to_us++;
    b.currently_in_qso_with = my_base;
    b.currently_in_qso_since_ms = now_ms;
  } else if (!target_call.isEmpty ()) {
    b.times_replying_others++;
    QString new_partner = Radio::base_callsign (target_call);
    if (b.currently_in_qso_with != new_partner) {
      b.currently_in_qso_with = new_partner;
      b.currently_in_qso_since_ms = now_ms;
    }
  }
}

void StationTracker::note_we_called (QString const& sender_call, qint64 now_ms)
{
  QString base = Radio::base_callsign (sender_call);
  if (base.length () < 3) return;
  Behavior& b = data_[base];
  b.times_we_called++;
  b.last_we_called_ms = now_ms;
}

void StationTracker::note_qso_completed (QString const& sender_call)
{
  QString base = Radio::base_callsign (sender_call);
  if (base.length () < 3) return;
  Behavior& b = data_[base];
  b.times_we_completed++;
  b.times_observed_completing++;
  b.currently_in_qso_with.clear ();
}

void StationTracker::set_continent (QString const& sender_call, QString const& continent_code)
{
  QString base = Radio::base_callsign (sender_call);
  if (base.length () < 3) return;
  data_[base].continent = continent_code.toUpper ().trimmed ();
}

void StationTracker::set_distance (QString const& sender_call, int km)
{
  QString base = Radio::base_callsign (sender_call);
  if (base.length () < 3) return;
  data_[base].distance_km = km;
}

StationTracker::Behavior const& StationTracker::get (QString const& base_call) const
{
  auto it = data_.find (base_call);
  if (it == data_.end ()) return empty_;
  return it.value ();
}

bool StationTracker::is_busy_now (QString const& base_call, qint64 now_ms) const
{
  auto it = data_.find (base_call);
  if (it == data_.end ()) return false;
  Behavior const& b = it.value ();
  if (b.currently_in_qso_with.isEmpty ()) return false;
  // If the QSO state is older than 90 seconds with no fresh decode, it's
  // probably stale.
  if (now_ms - b.last_heard_ms > 90000) return false;
  if (now_ms - b.currently_in_qso_since_ms > 180000) return false;  // 3 min cap
  return true;
}

void StationTracker::prune (qint64 now_ms, int keep_minutes)
{
  qint64 cutoff = now_ms - qint64 (keep_minutes) * 60000;
  QMutableHashIterator<QString, Behavior> it (data_);
  while (it.hasNext ()) {
    it.next ();
    if (it.value ().last_heard_ms < cutoff) it.remove ();
  }
}
