#include <iostream>
#include <JuceHeader.h>
#include <Windows.h>

#include "Medley.h"

using namespace juce;
using namespace medley;

class Track : public medley::ITrack {
public:
    Track(File& file)
        :
        file(file)
    {

    }

    File& getFile() override {
        return file;
    }

private:
    JUCE_LEAK_DETECTOR(Track)

    File file;
};

class Queue : public medley::IQueue {
public:
    size_t count() const override {
        return tracks.size();
    }

    medley::ITrack::Ptr fetchNextTrack() {
        auto track = tracks.front();
        tracks.erase(tracks.begin());
        return track;
    }

    std::list<Track::Ptr> tracks;
};

class MedleyApp : public JUCEApplication {
public:
    void initialise(const String& commandLine) override
    {
        myMainWindow.reset(new MainWindow());
        myMainWindow->setVisible(true);
    }

    void shutdown() override {
        myMainWindow = nullptr;
    }

    const juce::String getApplicationName() override { return "Medley Playground"; }

    const juce::String getApplicationVersion() override { return "0.1.0"; }

private:

    class PlayHead : public Component {
    public:
        PlayHead(Deck* deck, Deck* anotherDeck)
            :
            deck(deck),
            anotherDeck(anotherDeck)
        {

        }

        void updateDecks(Deck* deck, Deck* anotherDeck) {
            this->deck = deck;
            this->anotherDeck = anotherDeck;
        }      

        void paint(Graphics& g) override {
            auto w = (float)getWidth();
            auto h = (float)getHeight();

            if (!deck->isTrackLoaded()) {
                return;
            }

            // container
            g.setColour(Colours::lightgrey.darker(0.22));
            g.fillRect(0.0f, 0.0f, w, h);

            // progress
            auto pos = (float)deck->getPositionInSeconds();
            auto duration = deck->getDuration();

            if (duration <= 0) {
                return;
            }

            g.setColour(Colours::green);
            g.fillRect(0.0f, 0.0f, (pos / duration) * w, h);

            auto sr = deck->getSourceSampleRate();
            auto first = deck->getFirstAudiblePosition();
            auto last = deck->getEndPosition();

            auto leading = deck->getLeadingSamplePosition() / sr;
            auto trailing = deck->getTrailingSamplePosition() / sr;

            auto nextLeading = (anotherDeck->isTrackLoaded() ? anotherDeck->getLeadingDuration() : 0);
            //
            auto cuePoint = deck->getTransitionCuePosition();
            auto transitionStart = deck->getTransitionStartPosition() - nextLeading;
            auto transitionEnd = deck->getTransitionEndPosition();

            g.fillCheckerBoard(juce::Rectangle(0.0f, 0.0f, (float)(first / duration * w), h), 4, 4, Colours::darkgrey, Colours::darkgrey.darker());
            g.fillCheckerBoard(juce::Rectangle((float)(last / duration * w), 0.0f, w, h), 4, 4, Colours::darkgrey, Colours::darkgrey.darker());

            // cue
            g.setColour(Colours::yellow);
            g.drawVerticalLine(cuePoint / duration * w, 0, h);

            // transition
            {
                g.setGradientFill(ColourGradient(
                    Colours::hotpink.withAlpha(0.4f), transitionStart / duration * w, 0,
                    Colours::lightpink.withAlpha(0.7f), transitionEnd / duration * w, 0,
                    false
                ));
                g.fillRect(
                    transitionStart / duration * w, 0.0f,
                    (jmax(transitionEnd, last) - transitionStart) / duration * w, h
                );
            }

            // leading
            g.setColour(Colours::palevioletred);
            g.drawVerticalLine(leading / duration * w, 0, w);

            // trailing
            g.setColour(Colours::orangered);
            g.drawVerticalLine(trailing / duration * w, 0, w);
        }

        void mouseDown(const MouseEvent& event) override {
            deck->setPositionFractional((double)event.getMouseDownX() / getWidth());
        }

        medley::Deck* deck;
        medley::Deck* anotherDeck;
    };
    

    class DeckComponent : public Component, public Deck::Callback {
    public:
        DeckComponent(Deck& deck, Deck& anotherDeck)
            :
            deck(deck),
            playhead(&deck, &anotherDeck)
        {
            deck.addListener(this);

            addAndMakeVisible(playhead);
        }

        ~DeckComponent() override {
            deck.removeListener(this);
        }

        void deckTrackScanning(Deck& sender) override {

        }

        void deckTrackScanned(Deck& sender) override  {

        }

        void deckPosition(Deck& sender, double position) override {

        }

        void deckStarted(Deck& sender) override {

        }

        void deckFinished(Deck& sender) override {

        }

        void deckLoaded(Deck& sender) override {
            
        }

        void deckUnloaded(Deck& sender) override {

        }

        void resized() {
            auto b = getLocalBounds();
            playhead.setBounds(b.removeFromBottom(24).reduced(4, 4));
        }

        void paint(Graphics& g) override {
            g.setColour(Colours::lightgrey);
            g.fillRect(0, 0, getWidth(), getHeight());
        }

        medley::Deck& deck;
        PlayHead playhead;
    };

    class QueueModel : public ListBoxModel {
    public:
        QueueModel(Queue& queue)
            : queue(queue)
        {
            
        }

        int getNumRows() override
        {
            return queue.count();
        }

        void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override {
            if (rowIsSelected) {
                g.fillAll(Colours::lightblue);
            }

            g.setColour(LookAndFeel::getDefaultLookAndFeel().findColour(Label::textColourId));            

            auto at = std::next(queue.tracks.begin(), rowNumber);
            if (at != queue.tracks.end()) {
                g.drawText(at->get()->getFile().getFullPathName(), 0, 0, width, height, Justification::centredLeft, false);
            }
        }

    private:
        Queue& queue;
    };

    class MainContentComponent : public Component, public Timer, public Button::Listener, public medley::Medley::Callback {
    public:
        MainContentComponent() :
            Component(),
            model(queue),
            medley(queue),
            queueListBox({}, &model),
            btnOpen("Add")
        {
            medley.addListener(this);

            deckA = new DeckComponent(medley.getDeck1(), medley.getDeck2());
            addAndMakeVisible(deckA);

            deckB = new DeckComponent(medley.getDeck2(), medley.getDeck1());
            addAndMakeVisible(deckB);

            btnOpen.addListener(this);
            addAndMakeVisible(btnOpen);

            playhead = new PlayHead(&medley.getDeck1(), &medley.getDeck2());
            addAndMakeVisible(playhead);

            queueListBox.setColour(ListBox::outlineColourId, Colours::grey);
            addAndMakeVisible(queueListBox);

            setSize(800, 600);

            startTimerHz(20);
        }

        void timerCallback() override {
            deckA->repaint();
            deckB->repaint();
            playhead->repaint();
        }

        void resized() override {
            auto b = getLocalBounds();
            {
                auto deckPanelArea = b.removeFromTop(200).reduced(10, 0);
                auto w = (deckPanelArea.getWidth() - 10) / 2;
                deckA->setBounds(deckPanelArea.removeFromLeft(w));
                deckB->setBounds(deckPanelArea.translated(10, 0).removeFromLeft(w));
            }
            {
                auto controlArea = b.removeFromTop(32).translated(0, 4).reduced(10, 4);
                btnOpen.setBounds(controlArea.removeFromLeft(55));
                playhead->setBounds(controlArea.translated(4, 0).reduced(4, 0));
            }
            {
                queueListBox.setBounds(b.reduced(10));
            }
        }

        ~MainContentComponent() {
            medley.removeListener(this);

            removeChildComponent(deckA);
            removeChildComponent(deckB);
            removeChildComponent(playhead);

            delete deckA;
            delete deckB;
            delete playhead;
        }

        void buttonClicked(Button*) override {
            FileChooser fc("test");

            if (fc.browseForMultipleFilesToOpen()) {
                auto files = fc.getResults();

                for (auto f : files) {
                    queue.tracks.push_back(new Track(f));
                }                

                medley.play();
                queueListBox.updateContent();
            }
        }

        void deckTrackScanning(Deck& sender) override {

        }

        void deckTrackScanned(Deck& sender) override {

        }

        void deckPosition(Deck& sender, double position) override {
            
        }

        void deckStarted(Deck& sender) override {

        }

        void deckFinished(Deck& sender) override {

        }

        void deckLoaded(Deck& sender) override {
            if (auto deck = medley.getActiveDeck()) {
                auto anotherDeck = medley.getAnotherDeck(deck);
                playhead->updateDecks(deck, anotherDeck);
            }

            updateQueueListBox();
        }

        void deckUnloaded(Deck& sender) override {
            if (auto deck = medley.getActiveDeck()) {
                auto anotherDeck = medley.getAnotherDeck(deck);
                playhead->updateDecks(deck, anotherDeck);
            }
        }

        void updateQueueListBox() {
            const MessageManagerLock mml(Thread::getCurrentThread());
            if (mml.lockWasGained()) {
                queueListBox.deselectAllRows();
                queueListBox.updateContent();
            }
        }

        TextButton btnOpen;
        ListBox queueListBox;

        PlayHead* playhead = nullptr;

        DeckComponent* deckA = nullptr;
        DeckComponent* deckB = nullptr;

        Queue queue;
        QueueModel model;
        medley::Medley medley;
    };

    class MainWindow : public DocumentWindow {
    public:
        explicit MainWindow()
            : DocumentWindow("Medley Playground", Colours::white, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainContentComponent(), true);
            setBounds(100, 50, 800, 600);
            setResizable(true, false);
            setVisible(true);

            LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName("Tahoma");
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }        
    };

    std::unique_ptr<MainWindow> myMainWindow;
};

juce::JUCEApplicationBase* createApplication() {
    return new MedleyApp();
}

int main()
{
    static_cast<void>(::CoInitialize(nullptr));

    juce::JUCEApplicationBase::createInstance = &createApplication;
    return juce::JUCEApplicationBase::main();

    static_cast<void>(getchar());
}