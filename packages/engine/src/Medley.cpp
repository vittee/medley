#include "Medley.h"

namespace medley {

Medley::Medley(IQueue& queue)
    :
    queue(queue),
    loadingThread("Loading Thread"),
    readAheadThread("Read-ahead-thread")
{
    updateFadingFactor();

    deviceMgr.initialise(0, 2, nullptr, true, {}, nullptr);
    formatMgr.registerBasicFormats();

    deck1 = new Deck("Deck A", formatMgr, loadingThread, readAheadThread);
    deck2 = new Deck("Deck B", formatMgr, loadingThread, readAheadThread);

    deck1->addListener(this);
    deck2->addListener(this);

    loadingThread.startThread();
    readAheadThread.startThread(8);

    mixer.addInputSource(deck1, false);
    mixer.addInputSource(deck2, false);

    mainOut.setSource(&mixer);
    deviceMgr.addAudioCallback(&mainOut);
}

Medley::~Medley() {
    deck1->removeListener(this);
    deck2->removeListener(this);
    //
    mixer.removeAllInputs();
    mainOut.setSource(nullptr);

    loadingThread.stopThread(100);
    readAheadThread.stopThread(100);

    deviceMgr.closeAudioDevice();

    delete deck1;
    delete deck2;
}

bool Medley::loadNextTrack(Deck* currentDeck, bool play) {
    auto deck = getAnotherDeck(currentDeck);

    if (deck == nullptr) {
        DBG("Could not find another deck for " + getDeckName(*currentDeck));
        return false;
    }

    while (queue.count() > 0) {
        auto track = queue.fetchNextTrack();
        if (deck->loadTrack(track, play)) {
            return true;
        }
    }

    return false;
}

void Medley::deckTrackScanning(Deck& sender)
{

}

void Medley::deckTrackScanned(Deck& sender)
{

}

Deck* Medley::getAvailableDeck() {
    return !deck1->isTrackLoaded() ? deck1 : (!deck2->isTrackLoaded() ? deck2 : nullptr);
}

Deck* Medley::getAnotherDeck(Deck* from) {
    if (from == nullptr) {
        return getAvailableDeck();
    }

    return (from == deck1) ? deck2 : deck1;
}

inline String Medley::getDeckName(Deck& deck) {
    return deck.getName();
}

void Medley::deckStarted(Deck& sender) {
    DBG(String::formatted("[deckStarted] %s", sender.getName().toWideCharPointer()));

    ScopedLock sl(callbackLock);
    listeners.call([&sender](Callback& cb) {
        cb.deckStarted(sender);
    });
}

void Medley::deckFinished(Deck& sender) {
    ScopedLock sl(callbackLock);
    listeners.call([&sender](Callback& cb) {
        cb.deckFinished(sender);
    });
}

void Medley::deckLoaded(Deck& sender)
{
    {
        ScopedLock sl(callbackLock);

        deckQueue.push_back(&sender);

        listeners.call([&](Callback& cb) {
            cb.deckLoaded(sender);
        });
    }
}

void Medley::deckUnloaded(Deck& sender) {
    if (&sender == transitingDeck) {
        if (transitionState == TransitionState::Cue) {
            DBG(String::formatted("[%s] stopped before transition would happen, try starting next deck", sender.getName().toWideCharPointer()));
            auto nextDeck = getAnotherDeck(transitingDeck);
            if (nextDeck->isTrackLoaded()) {
                nextDeck->start();
            }
        }

        transitionState = TransitionState::Idle;
        transitingDeck = nullptr;
    }

    {
        ScopedLock sl(callbackLock);

        deckQueue.remove(&sender);

        listeners.call([&](Callback& cb) {
            cb.deckUnloaded(sender);
        });
    }

    // Just in case
    if (playing && !isDeckPlaying()) {
        auto shouldContinuePlaying = queue.count() > 0;
        playing = shouldContinuePlaying;

        if (shouldContinuePlaying) {
            loadNextTrack(nullptr, true);
        }
    }
}

void Medley::deckPosition(Deck& sender, double position) {
    {
        ScopedLock sl(callbackLock);
        listeners.call([&](Callback& cb) {
            cb.deckPosition(sender, position);
        });
    }

    auto nextDeck = getAnotherDeck(&sender);
    if (nextDeck == nullptr) {
        return;
    }

    auto transitionCuePoint = sender.getTransitionCuePosition();
    auto transitionStartPos = sender.getTransitionStartPosition();
    auto transitionEndPos = sender.getTransitionEndPosition();
    auto trailingDuration = sender.getTrailingDuration();

    auto leadingDuration = nextDeck->getLeadingDuration();

    if (transitionState == TransitionState::Idle) {
        if (position > transitionCuePoint) {
            if (!loadNextTrack(&sender, false)) {
                // No more track, do not transit
                return;
            }

            DBG(String::formatted("[%s] cue", nextDeck->getName().toWideCharPointer()));
            transitionState = TransitionState::Cue;
            transitingDeck = &sender;
        }
    }

    if (position > transitionStartPos - leadingDuration) {
        if (transitionState != TransitionState::Transit) {
            if (nextDeck->isTrackLoaded()) {
                DBG(String::formatted("Transiting to [%s]", nextDeck->getName().toWideCharPointer()));
                transitionState = TransitionState::Transit;                
                nextDeck->setVolume(1.0f);
                nextDeck->start();
            }
        }

        if (transitionState == TransitionState::Transit) {
            if (leadingDuration >= longLeadingTrackDuration) {
                auto fadeInProgress = jlimit(0.25, 1.0, (position - (transitionStartPos - leadingDuration)) / leadingDuration);

                DBG(String::formatted("[%s] Fading in: %.2f", nextDeck->getName().toWideCharPointer(), fadeInProgress));
                nextDeck->setVolume((float)pow(fadeInProgress, fadingFactor));
            }
        }
    }

    if (position >= transitionStartPos) {
        auto transitionDuration = (transitionEndPos - transitionStartPos);

        if (transitionDuration >= 2) { // TODO: Configurable
            auto transitionProgress = jlimit(0.0, 1.0, (position - transitionStartPos) / transitionDuration);

            DBG(String::formatted("[%s] Fading out: %.2f", sender.getName().toWideCharPointer(), transitionProgress));
            sender.setVolume((float)pow(1.0f - transitionProgress, fadingFactor));
        }
    }
}

Deck* Medley::getActiveDeck() const
{
    return deckQueue.empty() ? nullptr : deckQueue.front();
}

void Medley::setFadingCurve(double curve) {
    fadingCurve = jlimit(0.0, 100.0, curve);
    updateFadingFactor();
}

void Medley::play()
{
    if (!isDeckPlaying()) {
        loadNextTrack(nullptr, true);
    }

    playing = true;
}

bool Medley::isDeckPlaying()
{
    return deck1->isPlaying() || deck2->isPlaying();
}

void Medley::addListener(Callback* cb)
{
    ScopedLock sl(callbackLock);
    listeners.add(cb);
}

void Medley::removeListener(Callback* cb)
{
    ScopedLock sl(callbackLock);
    listeners.remove(cb);
}

void Medley::updateFadingFactor() {
    double outRange = 1000.0 - 1.0;
    double inRange = 100.0;
    fadingFactor = (float)(1000.0 / (((100.0 - fadingCurve) / inRange * outRange) + 1.0));
}

}