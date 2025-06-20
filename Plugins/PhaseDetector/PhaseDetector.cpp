/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2024 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "PhaseDetector.h"
#include "PhaseDetectorEditor.h"
#include <stdio.h>

PhaseDetectorSettings::PhaseDetectorSettings() : samplesSinceTrigger (0),
                                                 lastSample (0.0f),
                                                 isActive (true),
                                                 wasTriggered (false),
                                                 detectorType (PEAK),
                                                 currentPhase (NO_PHASE),
                                                 triggerChannel (0),
                                                 outputLine (0),
                                                 gateLine (-1),
                                                 lastOutputLine (0),
                                                 outputLineChanged (false),
                                                 lastTTLWord (0)
{
}

TTLEventPtr PhaseDetectorSettings::createEvent (int64 sample_number, bool state)
{
    TTLEventPtr event = TTLEvent::createTTLEvent (eventChannel,
                                                  sample_number,
                                                  outputLine,
                                                  state);

    if (state)
    {
        samplesSinceTrigger = 0;
        wasTriggered = true;
    }
    else
    {
        wasTriggered = false;
    }

    return event;
}

TTLEventPtr PhaseDetectorSettings::clearOutputLine (int64 sample_number)
{
    TTLEventPtr event = TTLEvent::createTTLEvent (eventChannel,
                                                  sample_number,
                                                  lastOutputLine,
                                                  false);

    outputLineChanged = false;

    return event;
}

PhaseDetector::PhaseDetector() : GenericProcessor ("Phase Detector")
{
}

void PhaseDetector::registerParameters()
{
    addSelectedChannelsParameter (Parameter::STREAM_SCOPE, "channel", "Channel", "The continuous channel to analyze", 1);

    addTtlLineParameter (Parameter::STREAM_SCOPE, "ttl_out", "TTL out", "The output TTL line", 16);

    addTtlLineParameter (Parameter::STREAM_SCOPE, "gate_line", "Gate line", "The input TTL line for gating the signal", 16, false, true);
    getStreamParameter ("gate_line")->currentValue = -1;

    addCategoricalParameter (Parameter::STREAM_SCOPE,
                             "phase",
                             "Phase",
                             "The phase for triggering the output",
                             { "PEAK",
                               "FALLING ZERO-CROSSING",
                               "TROUGH",
                               "RISING ZERO-CROSSING" },
                             0);
}

AudioProcessorEditor* PhaseDetector::createEditor()
{
    editor = std::make_unique<PhaseDetectorEditor> (this);

    return editor.get();
}

void PhaseDetector::parameterValueChanged (Parameter* param)
{
    if (param->getName().equalsIgnoreCase ("phase"))
    {
        settings[param->getStreamId()]->detectorType = DetectorType ((int) param->getValue());
    }
    else if (param->getName().equalsIgnoreCase ("channel"))
    {
        Array<var>* array = param->getValue().getArray();

        if (array->size() > 0)
        {
            int localIndex = int (array->getFirst());
            int globalIndex = getDataStream (param->getStreamId())->getContinuousChannels()[localIndex]->getGlobalIndex();
            settings[param->getStreamId()]->triggerChannel = globalIndex;
        }
        else
        {
            settings[param->getStreamId()]->triggerChannel = -1;
        }
    }
    else if (param->getName().equalsIgnoreCase ("ttl_out"))
    {
        settings[param->getStreamId()]->lastOutputLine = settings[param->getStreamId()]->outputLine;
        settings[param->getStreamId()]->outputLine = (int) param->getValue();
        settings[param->getStreamId()]->outputLineChanged = true;
    }
    else if (param->getName().equalsIgnoreCase ("gate_line"))
    {
        int newGateLine = (int) param->getValue();
        settings[param->getStreamId()]->gateLine = newGateLine;

        uint64 ttlWord = settings[param->getStreamId()]->lastTTLWord;
        // get the bit from the TTL word corresponding to the gate line and check if it is set
        if (newGateLine >= 0)
        {
            settings[param->getStreamId()]->isActive = ((ttlWord & (1ULL << newGateLine)) != 0);
        }
        else
        {
            settings[param->getStreamId()]->isActive = true; // If gate line is out of range, always active
        }

        LOGC ("Gate line set to ", newGateLine, " ttl word is ", String (ttlWord));
    }
}

void PhaseDetector::updateSettings()
{
    settings.update (getDataStreams());

    for (auto stream : getDataStreams())
    {
        EventChannel::Settings s {
            EventChannel::Type::TTL,
            "Phase detector output",
            "Triggers when the input signal meets a given phase condition",
            "dataderived.phase",
            getDataStream (stream->getStreamId())

        };

        eventChannels.add (new EventChannel (s));
        eventChannels.getLast()->addProcessor (this);
        settings[stream->getStreamId()]->eventChannel = eventChannels.getLast();

        settings[stream->getStreamId()]->lastTTLWord = 0;

        parameterValueChanged (stream->getParameter ("phase"));
        parameterValueChanged (stream->getParameter ("channel"));
        parameterValueChanged (stream->getParameter ("ttl_out"));
        parameterValueChanged (stream->getParameter ("gate_line"));
    }
}

void PhaseDetector::handleTTLEvent (TTLEventPtr event)
{
    const uint16 eventStream = event->getStreamId();
    settings[eventStream]->lastTTLWord = event->getWord();

    if (settings[eventStream]->gateLine > -1)
    {
        if (settings[eventStream]->gateLine == event->getLine())
            settings[eventStream]->isActive = event->getState();
    }
}

void PhaseDetector::process (AudioBuffer<float>& buffer)
{
    checkForEvents();

    // loop through the streams
    for (auto stream : getDataStreams())
    {
        if ((*stream)["enable_stream"])
        {
            PhaseDetectorSettings* module = settings[stream->getStreamId()];

            const uint16 streamId = stream->getStreamId();
            const int64 firstSampleInBlock = getFirstSampleNumberForBlock (streamId);
            const uint32 numSamplesInBlock = getNumSamplesInBlock (streamId);

            // check to see if it's active and has a channel
            if (module->isActive && module->outputLine >= 0
                && module->triggerChannel >= 0
                && module->triggerChannel < buffer.getNumChannels())
            {
                for (int i = 0; i < numSamplesInBlock; ++i)
                {
                    const float sample = *buffer.getReadPointer (module->triggerChannel, i);

                    if (sample < module->lastSample
                        && sample > 0
                        && module->currentPhase != FALLING_POS)
                    {
                        if (module->detectorType == PEAK)
                        {
                            TTLEventPtr ptr = module->createEvent (
                                firstSampleInBlock + i,
                                true);

                            addEvent (ptr, i);

                            //LOGD("PEAK");
                        }

                        module->currentPhase = FALLING_POS;
                    }
                    else if (sample < 0
                             && module->lastSample >= 0
                             && module->currentPhase != FALLING_NEG)
                    {
                        if (module->detectorType == FALLING_ZERO)
                        {
                            TTLEventPtr ptr = module->createEvent (
                                firstSampleInBlock + i,
                                true);

                            addEvent (ptr, i);

                            //("FALLING ZERO");
                        }

                        module->currentPhase = FALLING_NEG;
                    }
                    else if (sample > module->lastSample
                             && sample < 0
                             && module->currentPhase != RISING_NEG)
                    {
                        if (module->detectorType == TROUGH)
                        {
                            TTLEventPtr ptr = module->createEvent (
                                firstSampleInBlock + i,
                                true);

                            addEvent (ptr, i);

                            //LOGD("TROUGH");
                        }

                        module->currentPhase = RISING_NEG;
                    }
                    else if (sample > 0
                             && module->lastSample <= 0
                             && module->currentPhase != RISING_POS)
                    {
                        if (module->detectorType == RISING_ZERO)
                        {
                            TTLEventPtr ptr = module->createEvent (
                                firstSampleInBlock + i,
                                true);

                            addEvent (ptr, i);

                            //LOGD("RISING ZERO");
                        }

                        module->currentPhase = RISING_POS;
                    }

                    module->lastSample = sample;

                    if (module->wasTriggered)
                    {
                        if (module->samplesSinceTrigger > 2000)
                        {
                            TTLEventPtr ptr = module->createEvent (
                                firstSampleInBlock + i,
                                false);

                            addEvent (ptr, i);

                            //LOGD("TURNING OFF");
                        }
                        else
                        {
                            module->samplesSinceTrigger++;
                        }
                    }

                    if (module->outputLineChanged)
                    {
                        TTLEventPtr ptr = module->clearOutputLine (
                            firstSampleInBlock + i);

                        addEvent (ptr, i);
                    }
                }
            }

            // If event is on when 'None' is selected in channel selector, turn off event
            if (module->wasTriggered && (module->triggerChannel < 0 || module->isActive == false))
            {
                TTLEventPtr ptr = module->createEvent (firstSampleInBlock, false);

                addEvent (ptr, 0);
            }
        }
    }
}
