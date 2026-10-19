/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>

class PeerData;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class Show;
} // namespace Ui

namespace NovaGram::Forbidden {

// Checking content against the public lists of persons and organisations that
// Russian law forbids to quote or promote.
//
// Everything here is local. The lists are one public JSON file; the client
// fetches it at most once a day and never sends anything about its owner, the
// chat or the message. A bundled snapshot sits beside the executable so the
// first start works offline. Matching is a hash probe against an index built
// once per load - nothing runs while the user types or scrolls.
//
// Only the "иноагенты" list carries links, so only it matches by peer id,
// username and URL. The other three people/organisation lists are matched by
// the quoted name of the organisation against the title of a peer, which is
// weaker and said so in the docs. The fifth list (extremist materials) has no
// links; it is matched by the full title of a material inside a text.
enum class List : unsigned char {
	Agent = 0,
	Terrorist = 1,
	Extremist = 2,
	Undesirable = 3,
	// Not a list of people or organisations: the federal list of extremist
	// materials. It has no links and is matched by the full title of a
	// material found in a text.
	Materials = 4,
};

// On by default: it only marks and warns, and the one request it makes is a
// fixed public file. Off is a stored false and removes every effect (I8).
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

[[nodiscard]] QString Title();
[[nodiscard]] QString About();

// Loads the cached or bundled index and starts the once-a-day sync. Does
// nothing in the decoy.
void Start();

// Bumped whenever the index or the switch changes, so a per-item cached verdict
// knows when it is stale.
[[nodiscard]] uint32 Generation();

[[nodiscard]] std::optional<List> ForPeer(not_null<PeerData*> peer);

// The author, or the original author of a forward. Not the chat the item sits in.
[[nodiscard]] std::optional<List> ForItem(not_null<HistoryItem*> item);

// Exact links of a listed resource, or the full title of a listed material,
// found in a text. A link wins over a title. For a title `link` is the title
// as the list spells it.
struct TextHit {
	List list = List::Agent;
	QString link;
};
[[nodiscard]] std::optional<TextHit> ForText(const QString &text);

// Draws the mark over an already painted bubble.
void PaintMark(QPainter &p, const QRect &bubble);

// Warnings. All of them pass straight through when the setting is off or
// nothing is flagged.
//
// Quote and reply-with-quote: `proceed` runs at once or after "Continue".
void ConfirmQuote(
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item,
	Fn<void()> proceed);

// Forward and send re-enter the function that asked, so they answer whether a
// box was shown (the caller must then return) and let the next call through.
[[nodiscard]] bool ConfirmForward(
	std::shared_ptr<Ui::Show> show,
	not_null<Main::Session*> session,
	const std::vector<FullMsgId> &ids,
	Fn<void()> proceed);

[[nodiscard]] bool ConfirmSend(
	std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer,
	const QString &text,
	Fn<void()> proceed);

// Called from the "Continue" of those two, right before re-entering.
void PassOnce();

} // namespace NovaGram::Forbidden
