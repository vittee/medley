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
    auto audioSetup = deviceMgr.getAudioDeviceSetup();
    audioSetup.bufferSize = (int)(audioSetup.sampleRate * 0.25);
    deviceMgr.setAudioDeviceSetup(audioSetup, false);

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

    if (queue.count() <= 0) {
        return false;
    }

    auto track = queue.fetchNextTrack();
    deck->loadTrack(track->getFullPath(), play);
    return true;
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
    ScopedLock sl(callbackLock);
    listeners.call([&sender](Callback& cb) {
        cb.deckLoaded(sender);
    });
}

void Medley::deckUnloaded(Deck& sender) {
    if (&sender == transitingDeck) {
        transitionState = TransitionState::Idle;
    }

    {
        ScopedLock sl(callbackLock);
        listeners.call([&](Callback& cb) {
            cb.deckUnloaded(sender);
        });
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

    if (transitionState == TransitionState::Idle) {
        if (position > sender.getTransitionCuePosition()) {
            if (!loadNextTrack(&sender, false)) {
                // No more track, do not transit
                return;
            }

            DBG("CUE NEXT");
            transitionState = TransitionState::Cue;
        }
    }

    auto transitionStartPos = sender.getTransitionStartPosition();
    auto transitionEndPos = sender.getTransitionEndPosition();

    if (position > transitionStartPos) {
        if (transitionState != TransitionState::Transit) {
            if (nextDeck->isTrackLoaded()) {
                DBG("TRANSIT");
                transitionState = TransitionState::Transit;
                transitingDeck = &sender;
                nextDeck->start();
            }
        }

        auto transitionProgress = jlimit(0.0, 1.0, (position - transitionStartPos) / (transitionEndPos - transitionStartPos));
        sender.setVolume((float)pow(1.0f - transitionProgress, fadingFactor));
    }
}

void Medley::setFadingCurve(double curve) {
    fadingCurve = jlimit(0.0, 100.0, curve);
    updateFadingFactor();
}

void Medley::play()
{
    if (!isPlaying()) {
        loadNextTrack(nullptr, true);
    }
}

bool Medley::isPlaying()
{
    return deck1->isPlaying() || deck2->isPlaying();
}

void Medley::addListener(Callback* cb)
{
    ScopedLock sl(callbackLock);
    listeners.add(cb);
}

void Medley::updateFadingFactor() {
    double outRange = 1000.0 - 1.0;
    double inRange = 100.0;
    fadingFactor = (float)(1000.0 / (((100.0 - fadingCurve) / inRange * outRange) + 1.0));
}

}
