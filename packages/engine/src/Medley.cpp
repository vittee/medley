#include "Medley.h"
#include "MiniMP3AudioFormat.h"

#if JUCE_WINDOWS
#include <Windows.h>
#endif

namespace medley {

Medley::Medley(IQueue& queue)
    :
    mixer(*this),
    queue(queue),
    loadingThread("Loading Thread"),
    readAheadThread("Read-ahead-thread"),
    visualizingThread("Visualizing Thread")
{
#if JUCE_WINDOWS
    static_cast<void>(::CoInitialize(nullptr));
#endif

    updateFadingFactor();

    auto error = deviceMgr.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty()) {
        throw std::runtime_error(error.toStdString());
    }

    mixer.updateAudioConfig();

    deviceMgr.addChangeListener(&mixer);

    formatMgr.registerFormat(new MiniMP3AudioFormat(), true);
    formatMgr.registerFormat(new WavAudioFormat(), false);
    formatMgr.registerFormat(new AiffAudioFormat(), false);
    formatMgr.registerFormat(new FlacAudioFormat(), false);
    formatMgr.registerFormat(new OggVorbisAudioFormat(), false);

#if JUCE_MAC || JUCE_IOS
    formatMgr.registerFormat(new CoreAudioFormat(), false);
#endif

#if JUCE_USE_WINDOWS_MEDIA_FORMAT
    formatMgr.registerFormat(new WindowsMediaAudioFormat(), false);
#endif

    deck1 = new Deck("Deck A", formatMgr, loadingThread, readAheadThread);
    deck2 = new Deck("Deck B", formatMgr, loadingThread, readAheadThread);

    deck1->addListener(this);
    deck2->addListener(this);

    loadingThread.startThread();
    readAheadThread.startThread(8);
    visualizingThread.startThread();

    mixer.addInputSource(deck1, false);
    mixer.addInputSource(deck2, false);

    visualizingThread.addTimeSliceClient(&mixer);

    mainOut.setSource(&mixer);
    deviceMgr.addAudioCallback(&mainOut);
    deviceMgr.addChangeListener(this);

    if (auto device = deviceMgr.getCurrentAudioDevice()) {
        if (!device->isOpen()) {
            throw std::runtime_error("Audio device is not open");
        }

        if (!device->isPlaying()) {
            throw std::runtime_error("Audio device is not playing");
        }
    }
}

Medley::~Medley() {
    deck1->removeListener(this);
    deck2->removeListener(this);
    //
    mixer.removeAllInputs();
    mainOut.setSource(nullptr);

    loadingThread.stopThread(100);
    readAheadThread.stopThread(100);
    visualizingThread.stopThread(100);

    deviceMgr.closeAudioDevice();

    delete deck1;
    delete deck2;
}

void Medley::setPositionInSeconds(double time) {
    if (auto deck = getMainDeck()) {
        deck->setPosition(time);
    }
}

void Medley::setPositionFractional(double fraction)
{
    if (auto deck = getMainDeck()) {
        deck->setPositionFractional(fraction);
    }
}

double Medley::getDuration() const
{
    if (auto deck = getMainDeck()) {
        return deck->getDuration();
    }

    return 0.0;
}

double Medley::getPositionInSeconds() const
{
    if (auto deck = getMainDeck()) {
        return deck->getPositionInSeconds();
    }

    return 0.0;
}

void Medley::setMaxTransitionTime(double value) {
    maxTransitionTime = value;
    deck1->setMaxTransitionTime(value);
    deck2->setMaxTransitionTime(value);
}

void Medley::fadeOutMainDeck()
{
    if (auto deck = getMainDeck()) {
        forceFadingOut++;

        if (transitionState == TransitionState::Transit) {
            deck->unloadTrack();
            transitionState = TransitionState::Idle;

            deck = getMainDeck();
        }

        if (deck) {
            deck->fadeOut();
            mixer.setPause(false);
        }
    }
}

void Medley::changeListenerCallback(ChangeBroadcaster* source)
{
    if (auto deviceMgr = dynamic_cast<AudioDeviceManager*>(source)) {
        ScopedLock sl(callbackLock);

        listeners.call([](Callback& cb) {
            cb.audioDeviceChanged();
        });
    }
}

bool Medley::loadNextTrack(Deck* currentDeck, bool play) {
    auto deck = getAnotherDeck(currentDeck);

    if (deck == nullptr) {
        Logger::writeToLog("Could not find another deck for " + currentDeck->getName());
        return false;
    }

    if (deck->isTrackLoading) {
        Logger::writeToLog("Deck is busy loading some track!!!");
        deck->unloadTrack();
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
    Logger::writeToLog(String::formatted("[deckStarted] %s", sender.getName().toWideCharPointer()));

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
        deckQueue.front()->markAsMain(true);

        listeners.call([&](Callback& cb) {
            cb.deckLoaded(sender);
        });
    }
}

void Medley::deckUnloaded(Deck& sender) {
    if (&sender == transitingDeck) {
        if (transitionState == TransitionState::Cued) {
            Logger::writeToLog(String::formatted("[%s] stopped before transition would happen, try starting next deck", sender.getName().toWideCharPointer()));
            auto nextDeck = getAnotherDeck(transitingDeck);
            if (nextDeck->isTrackLoaded()) {
                nextDeck->start();
            }
        }

        transitionState = TransitionState::Idle;
        transitingDeck = nullptr;

        if (forceFadingOut > 0) {
            forceFadingOut--;
        }
    }

    {
        ScopedLock sl(callbackLock);

        sender.markAsMain(false);
        deckQueue.remove(&sender);

        if (!deckQueue.empty()) {
            deckQueue.front()->markAsMain(true);
        }

        listeners.call([&](Callback& cb) {
            cb.deckUnloaded(sender);
        });
    }

    // Just in case
    if (keepPlaying && !isDeckPlaying()) {
        auto shouldContinuePlaying = queue.count() > 0;
        keepPlaying = shouldContinuePlaying;

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

    if (sender.isMain()) {
        auto transitionPreCuePoint = sender.getTransitionPreCuePosition();
        auto transitionCuePoint = sender.getTransitionCuePosition();
        auto transitionStartPos = sender.getTransitionStartPosition();
        auto transitionEndPos = sender.getTransitionEndPosition();

        auto leadingDuration = nextDeck->getLeadingDuration();

        if (transitionState < TransitionState::Cued) {
            if (transitionState == TransitionState::Idle && position > transitionPreCuePoint) {
                transitionState = TransitionState::Cueing;

                {
                    ScopedLock sl(callbackLock);

                    listeners.call([](Callback& cb) {
                        cb.preCueNext();
                    });
                }
            }

            if (position > transitionCuePoint) {
                if (!loadNextTrack(&sender, false)) {
                    // No more track, do not transit
                    if (forceFadingOut <= 0) {
                        return;
                    }
                }

                Logger::writeToLog(String::formatted("[%s] cue", nextDeck->getName().toWideCharPointer()));
                transitionState = TransitionState::Cued;
                transitingDeck = &sender;
            }

            if (!sender.isMain() && nextDeck->isTrackLoaded() && !nextDeck->isPlaying()) {
                nextDeck->fireFinishedCallback();
            }
        }

        if (position > transitionStartPos - leadingDuration) {
            if (transitionState == TransitionState::Cued) {
                if (nextDeck->isTrackLoaded()) {
                    Logger::writeToLog(String::formatted("Transiting to [%s]", nextDeck->getName().toWideCharPointer()));
                    transitionState = TransitionState::Transit;
                    nextDeck->setVolume(1.0f);

                    if (forceFadingOut > 0) {
                        if (leadingDuration >= maxLeadingDuration) {
                            nextDeck->setPosition(nextDeck->getFirstAudiblePosition() + leadingDuration - maxLeadingDuration);
                        }
                    }

                    nextDeck->start();
                }
            }

            if (transitionState == TransitionState::Transit) {
                if (leadingDuration >= maxLeadingDuration) {
                    auto fadeInProgress = jlimit(0.25, 1.0, (position - (transitionStartPos - leadingDuration)) / leadingDuration);

                    Logger::writeToLog(String::formatted("[%s] Fading in: %.2f", nextDeck->getName().toWideCharPointer(), fadeInProgress));
                    nextDeck->setVolume((float)pow(fadeInProgress, fadingFactor));
                }
            }
        }
    
        if (position >= transitionStartPos) {
            auto transitionDuration = (transitionEndPos - transitionStartPos);
            auto transitionProgress = jlimit(0.0, 1.0, (position - transitionStartPos) / transitionDuration);

            if (transitionDuration > 0.0) {
                Logger::writeToLog(String::formatted("[%s] Fading out: %.2f", sender.getName().toWideCharPointer(), transitionProgress));
                sender.setVolume((float)pow(1.0f - transitionProgress, fadingFactor));
            }

            if (transitionState != TransitionState::Idle && position > transitionEndPos) {
                if (transitionProgress >= 1.0) {
                    sender.stop();
                }
            }
        }
    }
    // Just in case
    else if (!deckQueue.empty()) {
        if (deckQueue.front() == &sender) {
            sender.markAsMain(true);
        }
    }
}

void Medley::setAudioDeviceByIndex(int index) {
    auto config = deviceMgr.getAudioDeviceSetup();
    config.outputDeviceName = getDeviceNames()[index];
    auto error = deviceMgr.setAudioDeviceSetup(config, true);
    if (error.isNotEmpty()) {
        throw std::runtime_error(error.toStdString());
    }
}

Deck* Medley::getMainDeck() const
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

    keepPlaying = true;
    mixer.setPause(false);
}

void Medley::stop()
{
    keepPlaying = false;

    deck1->stop();
    deck2->stop();

    deck1->unloadTrack();
    deck2->unloadTrack();
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

bool Medley::Mixer::togglePause() {
    return paused = !paused;
}

void Medley::Mixer::getNextAudioBlock(const AudioSourceChannelInfo& info) {
    if (!outputStarted) {
        outputStarted = true;
        Logger::writeToLog("Output started");
    }

    if (!stalled) {
        MixerAudioSource::getNextAudioBlock(info);

        if (paused) {
            for (int i = info.buffer->getNumChannels(); --i >= 0;) {
                info.buffer->applyGainRamp(i, info.startSample, jmin(256, info.numSamples), 1.0f, 0.0f);
            }

            stalled = true;
        }
    }
    else /* stalled */ {
        if (!paused) {
            MixerAudioSource::getNextAudioBlock(info);

            for (int i = info.buffer->getNumChannels(); --i >= 0;) {
                info.buffer->applyGainRamp(i, info.startSample, jmin(256, info.numSamples), 0.0f, 1.0f);
            }

            stalled = false;
        }
    }

    if (prepared) {
        AudioBlock<float> block(*info.buffer, (size_t)info.startSample);
        processor.process(ProcessContextReplacing<float>(block));

        levelTracker.process(*info.buffer);
    }
}

void Medley::Mixer::changeListenerCallback(ChangeBroadcaster* source) {
    updateAudioConfig();
}

int Medley::Mixer::useTimeSlice()
{
    levelTracker.update();
    return 5;
}

void Medley::Mixer::updateAudioConfig()
{
    auto& deviceMgr = medley.deviceMgr;
    if (auto device = deviceMgr.getCurrentAudioDevice()) {
        auto config = deviceMgr.getAudioDeviceSetup();

        int latencyInSamples = device->getOutputLatencyInSamples();

#ifdef JUCE_WINDOWS
        if (device->getTypeName() == "DirectSound") {
            latencyInSamples *= 16;
        }
#endif

        auto numSamples = device->getCurrentBufferSizeSamples();
        numChannels = device->getOutputChannelNames().size();

        processor.prepare({ config.sampleRate, (uint32)numSamples, (uint32)numChannels });

        levelTracker.prepare(
            numChannels,
            (int)config.sampleRate,
            latencyInSamples,
            10
        );

        prepared = true;
    }
}

}
