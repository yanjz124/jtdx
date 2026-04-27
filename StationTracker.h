// Tracks observable behavior of remote stations seen on the band.
//
// Per-callsign data updated on every decode. Used by passive-mode
// candidate selection to compute "best chance of QSO" scores.
//
// Distinct from QsoHistory: that handles QSO state-machine progression
// for *our* QSOs; this aggregates statistics across *all* their on-air
// activity so we can decide whether to call them in the first place.

#ifndef STATION_TRACKER_H
#define STATION_TRACKER_H

#include <QHash>
#include <QString>
#include <QSet>
#include <QStringList>

class StationTracker
{
public:
  struct Behavior {
    int total_decoded = 0;          // any message from this station
    int times_cqing = 0;            // CQ messages
    int times_replying_others = 0;  // they answered someone else
    int times_replied_to_us = 0;    // they answered us specifically
    int times_we_called = 0;        // we sent something to them this session
    int times_we_completed = 0;     // we logged a QSO with them this session
    int times_observed_completing = 0; // we saw them complete a QSO with anyone
    int sum_snr = 0;                // running sum, divide by total_decoded for avg
    int best_snr = -99;             // strongest report we got from them
    qint64 first_heard_ms = 0;
    qint64 last_heard_ms = 0;
    qint64 last_we_called_ms = 0;
    QString currently_in_qso_with;  // empty if not mid-QSO; otherwise the call they're working
    qint64 currently_in_qso_since_ms = 0;
    QString continent;              // 2-letter, populated from logbook on first sighting
    QString grid;                   // 4 or 6 char locator, last seen value
    int distance_km = 0;            // computed once continent/grid known
    bool is_cqing_now () const;     // CQ in last 30 sec
    int avg_snr () const;
    double reply_rate () const;     // fraction of times we called → they replied
  };

  // Update with one decoded message.
  void note_decode (QString const& sender_call, QString const& target_call,
                    QString const& my_base_call, bool is_cq, int snr,
                    QString const& grid_hint, qint64 now_ms);

  // We sent a transmission to this call.
  void note_we_called (QString const& sender_call, qint64 now_ms);

  // We completed a QSO with this call.
  void note_qso_completed (QString const& sender_call);

  // Set continent for this call (called by MainWindow after logbook lookup).
  void set_continent (QString const& sender_call, QString const& continent_code);

  // Set distance for this call (called by MainWindow after grid resolution).
  void set_distance (QString const& sender_call, int km);

  // Lookup. Returns a const ref or a default-constructed Behavior if absent.
  Behavior const& get (QString const& base_call) const;
  bool has (QString const& base_call) const { return data_.contains (base_call); }

  // Active in-QSO check: if a station was seen replying-to-others in the
  // last ~60s, they're probably still in that QSO. Helps Phase 5 (busy
  // detection without consuming retries).
  bool is_busy_now (QString const& base_call, qint64 now_ms) const;

  // Drop entries we haven't heard from in N minutes (housekeeping).
  void prune (qint64 now_ms, int keep_minutes = 120);

  // Reset everything (e.g., on band change — propagation history is
  // band-specific so previous-band counts shouldn't bias the new band).
  void clear () { data_.clear (); }

  // Diagnostic
  int size () const { return data_.size (); }

private:
  QHash<QString, Behavior> data_;
  static Behavior empty_;
};

#endif
