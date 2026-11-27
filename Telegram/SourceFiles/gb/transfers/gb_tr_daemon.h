/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QObject>

class QProcess;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace GB::Transfers {

inline constexpr auto kInvitePrefix = "gb://transfer/";

// What a gb://transfer/ link says about the offered file.
struct InviteInfo {
	QString link;
	QString id;
	QString token;
	QString name;
	int64 size = 0;
	bool lanOnly = false;
};
[[nodiscard]] std::optional<InviteInfo> ParseInvite(const QString &text);

struct ReceiveRow {
	QString id;
	QString name;
	QString state; // queued, running, waiting, paused, completed, failed
	QString error;
	QString route; // lan, internet, direct while running
	QString dir;
	QString finalPath;
	QString inviteId;
	int64 size = 0;
	int64 received = 0;
	float64 progress = 0.;
	float64 speed = 0.;
	float64 eta = 0.;
};

struct SendRow {
	QString id;
	QString name;
	QString path;
	QString invite;
	int64 size = 0;
	int64 served = 0;
	float64 speed = 0.;
};

// An incoming offer the user has not answered yet ("Later").
struct PendingRow {
	QString link;
	QString from;
	TimeId date = 0;
};

enum class EngineState : uchar {
	Missing,  // p2pd.exe not found
	Starting,
	Running,
	Failed,
};

// Windows Firewall rules for p2pd.exe (TCP 38492 for same Wi-Fi, UDP 41642
// for direct internet paths), checked with "p2pd firewall check".
enum class FirewallState : uchar {
	Unknown,
	Ok,
	Missing,
	Fixing,
};

struct Status {
	EngineState engine = EngineState::Starting;
	FirewallState firewall = FirewallState::Unknown;
	QString firewallProblem;
	QString engineError;
	QStringList lanAddrs;
	QString networkState; // off, connecting, needs_login, running, failed
	QString loginUrl;
	QString networkError;
	QString downloadDir;
};

// Runs GB's transfer engine (p2pd.exe) next to the app and talks to its
// local API. One per process; created on first use, stopped on quit.
class Daemon final : public QObject {
public:
	static Daemon &Instance();

	void ensureStarted();
	void restart();

	[[nodiscard]] const Status &status() const {
		return _status;
	}
	[[nodiscard]] const std::vector<ReceiveRow> &receiving() const {
		return _receiving;
	}
	[[nodiscard]] const std::vector<SendRow> &sending() const {
		return _sending;
	}
	[[nodiscard]] const std::vector<PendingRow> &pending() const {
		return _pending;
	}
	[[nodiscard]] rpl::producer<> updates() const {
		return _updates.events();
	}

	// While a Transfers page is visible, poll every second.
	void setWatching(bool watching);

	[[nodiscard]] bool internetEnabled() const {
		return _internet;
	}
	void setInternetEnabled(bool enabled);

	// A reusable Tailscale auth key ("join key") so a PC joins the test
	// network without a browser sign-in. Passed to p2pd as TS_AUTHKEY.
	[[nodiscard]] bool hasJoinKey() const {
		return !_joinKey.isEmpty();
	}
	void setJoinKey(const QString &key);

	// Re-reads the firewall rules (fast, no admin rights needed).
	FirewallState checkFirewall();
	// True once per install: the first-run "allow in firewall" question
	// has not been answered yet and the rules are missing.
	[[nodiscard]] bool shouldAskFirewall();
	void setFirewallAsked();
	// Runs "p2pd firewall add", which shows Windows' admin (UAC) prompt.
	void fixFirewall(Fn<void(bool ok, QString error)> done);

	void createOffer(
		const QString &path,
		Fn<void(SendRow)> done,
		Fn<void(QString)> fail);
	void deleteOffer(const QString &id);
	void receive(
		const QString &link,
		const QString &dir,
		Fn<void()> done,
		Fn<void(QString)> fail);
	void pause(const QString &id);
	void resume(const QString &id);
	void remove(const QString &id, bool deleteFile);

	// Incoming offers from chats.
	[[nodiscard]] bool wasHandled(const InviteInfo &info) const;
	void markHandled(const InviteInfo &info);
	void addPending(const QString &link, const QString &from, TimeId date);
	void removePending(const QString &link);
	[[nodiscard]] bool isReceiving(const InviteInfo &info) const;

private:
	Daemon();
	~Daemon();

	void stop();
	void tryConnect(int attempt);
	void startProcess();
	void poll();
	void schedulePoll();
	void request(
		const QByteArray &verb,
		const QString &path,
		const QByteArray &body,
		Fn<void(QByteArray)> done,
		Fn<void(QString)> fail);
	void applyStatus(const QByteArray &data);
	void applyReceiving(const QByteArray &data);
	void applySending(const QByteArray &data);
	void loadSettings();
	void saveSettings();
	void notify();

	std::unique_ptr<QNetworkAccessManager> _network;
	std::unique_ptr<QTimer> _timer;
	QProcess *_process = nullptr;
	QString _addr;
	QString _key;
	Status _status;
	std::vector<ReceiveRow> _receiving;
	std::vector<SendRow> _sending;
	std::vector<PendingRow> _pending;
	base::flat_map<QString, std::pair<int64, crl::time>> _servedSamples;
	QStringList _handled;
	QString _joinKey;
	bool _internet = false;
	bool _firewallAsked = false;
	bool _started = false;
	bool _polling = false;
	int _watching = 0;
	rpl::event_stream<> _updates;

};

[[nodiscard]] QString FormatRoute(const QString &route);

} // namespace GB::Transfers
