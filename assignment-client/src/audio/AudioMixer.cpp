//
//  AudioMixer.cpp
//  hifi
//
//  Created by Stephen Birarda on 8/22/13.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "Syssocket.h"
#include "Systime.h"
#include <math.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/socket.h>
#endif //_WIN32

#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/vector_angle.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>

#include <Logging.h>
#include <NodeList.h>
#include <Node.h>
#include <NodeTypes.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>
#include <StdDev.h>
#include <UUID.h>

#include "AudioRingBuffer.h"
#include "AudioMixerClientData.h"
#include "AvatarAudioRingBuffer.h"
#include "InjectedAudioRingBuffer.h"

#include "AudioMixer.h"

const short JITTER_BUFFER_MSECS = 12;
const short JITTER_BUFFER_SAMPLES = JITTER_BUFFER_MSECS * (SAMPLE_RATE / 1000.0);

const unsigned int BUFFER_SEND_INTERVAL_USECS = floorf((BUFFER_LENGTH_SAMPLES_PER_CHANNEL / (float) SAMPLE_RATE) * 1000 * 1000);

const int MAX_SAMPLE_VALUE = std::numeric_limits<int16_t>::max();
const int MIN_SAMPLE_VALUE = std::numeric_limits<int16_t>::min();

const char AUDIO_MIXER_LOGGING_TARGET_NAME[] = "audio-mixer";

void attachNewBufferToNode(Node *newNode) {
    if (!newNode->getLinkedData()) {
        newNode->setLinkedData(new AudioMixerClientData());
    }
}

AudioMixer::AudioMixer(const unsigned char* dataBuffer, int numBytes) :
    ThreadedAssignment(dataBuffer, numBytes)
{
    
}

void AudioMixer::addBufferToMixForListeningNodeWithBuffer(PositionalAudioRingBuffer* bufferToAdd,
                                                          AvatarAudioRingBuffer* listeningNodeBuffer) {
    float bearingRelativeAngleToSource = 0.0f;
    float attenuationCoefficient = 1.0f;
    int numSamplesDelay = 0;
    float weakChannelAmplitudeRatio = 1.0f;
    
    const int PHASE_DELAY_AT_90 = 20;
    
    if (bufferToAdd != listeningNodeBuffer) {
        // if the two buffer pointers do not match then these are different buffers
        
        glm::vec3 listenerPosition = listeningNodeBuffer->getPosition();
        glm::vec3 relativePosition = bufferToAdd->getPosition() - listeningNodeBuffer->getPosition();
        glm::quat inverseOrientation = glm::inverse(listeningNodeBuffer->getOrientation());
        
        float distanceSquareToSource = glm::dot(relativePosition, relativePosition);
        float radius = 0.0f;
        
        if (bufferToAdd->getType() == PositionalAudioRingBuffer::Injector) {
            InjectedAudioRingBuffer* injectedBuffer = (InjectedAudioRingBuffer*) bufferToAdd;
            radius = injectedBuffer->getRadius();
            attenuationCoefficient *= injectedBuffer->getAttenuationRatio();
        }
        
        if (radius == 0 || (distanceSquareToSource > radius * radius)) {
            // this is either not a spherical source, or the listener is outside the sphere
            
            if (radius > 0) {
                // this is a spherical source - the distance used for the coefficient
                // needs to be the closest point on the boundary to the source
                
                // ovveride the distance to the node with the distance to the point on the
                // boundary of the sphere
                distanceSquareToSource -= (radius * radius);
                
            } else {
                // calculate the angle delivery for off-axis attenuation
                glm::vec3 rotatedListenerPosition = glm::inverse(bufferToAdd->getOrientation()) * relativePosition;
                
                float angleOfDelivery = glm::angle(glm::vec3(0.0f, 0.0f, -1.0f),
                                                   glm::normalize(rotatedListenerPosition));
                
                const float MAX_OFF_AXIS_ATTENUATION = 0.2f;
                const float OFF_AXIS_ATTENUATION_FORMULA_STEP = (1 - MAX_OFF_AXIS_ATTENUATION) / 2.0f;
                
                float offAxisCoefficient = MAX_OFF_AXIS_ATTENUATION +
                    (OFF_AXIS_ATTENUATION_FORMULA_STEP * (angleOfDelivery / 90.0f));
                
                // multiply the current attenuation coefficient by the calculated off axis coefficient
                attenuationCoefficient *= offAxisCoefficient;
            }
            
            glm::vec3 rotatedSourcePosition = inverseOrientation * relativePosition;
            
            const float DISTANCE_SCALE = 2.5f;
            const float GEOMETRIC_AMPLITUDE_SCALAR = 0.3f;
            const float DISTANCE_LOG_BASE = 2.5f;
            const float DISTANCE_SCALE_LOG = logf(DISTANCE_SCALE) / logf(DISTANCE_LOG_BASE);
            
            // calculate the distance coefficient using the distance to this node
            float distanceCoefficient = powf(GEOMETRIC_AMPLITUDE_SCALAR,
                                             DISTANCE_SCALE_LOG +
                                             (0.5f * logf(distanceSquareToSource) / logf(DISTANCE_LOG_BASE)) - 1);
            distanceCoefficient = std::min(1.0f, distanceCoefficient);
            
            // multiply the current attenuation coefficient by the distance coefficient
            attenuationCoefficient *= distanceCoefficient;
            
            // project the rotated source position vector onto the XZ plane
            rotatedSourcePosition.y = 0.0f;
            
            // produce an oriented angle about the y-axis
            bearingRelativeAngleToSource = glm::orientedAngle(glm::vec3(0.0f, 0.0f, -1.0f),
                                                              glm::normalize(rotatedSourcePosition),
                                                              glm::vec3(0.0f, 1.0f, 0.0f));
            
            const float PHASE_AMPLITUDE_RATIO_AT_90 = 0.5;
            
            // figure out the number of samples of delay and the ratio of the amplitude
            // in the weak channel for audio spatialization
            float sinRatio = fabsf(sinf(glm::radians(bearingRelativeAngleToSource)));
            numSamplesDelay = PHASE_DELAY_AT_90 * sinRatio;
            weakChannelAmplitudeRatio = 1 - (PHASE_AMPLITUDE_RATIO_AT_90 * sinRatio);
        }
    }
    
    int16_t* sourceBuffer = bufferToAdd->getNextOutput();
    
    int16_t* goodChannel = (bearingRelativeAngleToSource > 0.0f)
        ? _clientSamples
        : _clientSamples + BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
    int16_t* delayedChannel = (bearingRelativeAngleToSource > 0.0f)
        ? _clientSamples + BUFFER_LENGTH_SAMPLES_PER_CHANNEL
        : _clientSamples;
    
    int16_t* delaySamplePointer = bufferToAdd->getNextOutput() == bufferToAdd->getBuffer()
        ? bufferToAdd->getBuffer() + RING_BUFFER_LENGTH_SAMPLES - numSamplesDelay
        : bufferToAdd->getNextOutput() - numSamplesDelay;
    
    for (int s = 0; s < BUFFER_LENGTH_SAMPLES_PER_CHANNEL; s++) {
        if (s < numSamplesDelay) {
            // pull the earlier sample for the delayed channel
            int earlierSample = delaySamplePointer[s] * attenuationCoefficient * weakChannelAmplitudeRatio;
            
            delayedChannel[s] = glm::clamp(delayedChannel[s] + earlierSample, MIN_SAMPLE_VALUE, MAX_SAMPLE_VALUE);
        }
        
        // pull the current sample for the good channel
        int16_t currentSample = sourceBuffer[s] * attenuationCoefficient;
        goodChannel[s] = glm::clamp(goodChannel[s] + currentSample, MIN_SAMPLE_VALUE, MAX_SAMPLE_VALUE);
        
        if (s + numSamplesDelay < BUFFER_LENGTH_SAMPLES_PER_CHANNEL) {
            // place the curernt sample at the right spot in the delayed channel
            int sumSample = delayedChannel[s + numSamplesDelay] + (currentSample * weakChannelAmplitudeRatio);
            delayedChannel[s + numSamplesDelay] = glm::clamp(sumSample, MIN_SAMPLE_VALUE, MAX_SAMPLE_VALUE);
        }
    }
}

void AudioMixer::prepareMixForListeningNode(Node* node) {
	NodeList* nodeList = NodeList::getInstance();
    
    AvatarAudioRingBuffer* nodeRingBuffer = ((AudioMixerClientData*) node->getLinkedData())->getAvatarAudioRingBuffer();
    
    // zero out the client mix for this node
    memset(_clientSamples, 0, sizeof(_clientSamples));
    
    // loop through all other nodes that have sufficient audio to mix
    for (NodeList::iterator otherNode = nodeList->begin(); otherNode != nodeList->end(); otherNode++) {
        if (otherNode->getLinkedData()) {
            
            AudioMixerClientData* otherNodeClientData = (AudioMixerClientData*) otherNode->getLinkedData();
            
            // enumerate the ARBs attached to the otherNode and add all that should be added to mix
            for (int i = 0; i < otherNodeClientData->getRingBuffers().size(); i++) {
                PositionalAudioRingBuffer* otherNodeBuffer = otherNodeClientData->getRingBuffers()[i];
                
                if ((*otherNode != *node
                     || otherNodeBuffer->getType() != PositionalAudioRingBuffer::Microphone
                     || nodeRingBuffer->shouldLoopbackForNode())
                    && otherNodeBuffer->willBeAddedToMix()) {
                    addBufferToMixForListeningNodeWithBuffer(otherNodeBuffer, nodeRingBuffer);
                }
            }
        }
    }
}


void AudioMixer::processDatagram(const QByteArray& dataByteArray, const HifiSockAddr& senderSockAddr) {
    // pull any new audio data from nodes off of the network stack
    if (dataByteArray[0] == PACKET_TYPE_MICROPHONE_AUDIO_NO_ECHO
        || dataByteArray[0] == PACKET_TYPE_MICROPHONE_AUDIO_WITH_ECHO
        || dataByteArray[0] == PACKET_TYPE_INJECT_AUDIO) {
        QUuid nodeUUID = QUuid::fromRfc4122(dataByteArray.mid(numBytesForPacketHeader((unsigned char*) dataByteArray.data()),
                                                              NUM_BYTES_RFC4122_UUID));
        
        NodeList* nodeList = NodeList::getInstance();
        
        Node* matchingNode = nodeList->nodeWithUUID(nodeUUID);
        
        if (matchingNode) {
            nodeList->updateNodeWithData(matchingNode, senderSockAddr, (unsigned char*) dataByteArray.data(), dataByteArray.size());
            
            if (!matchingNode->getActiveSocket()) {
                // we don't have an active socket for this node, but they're talking to us
                // this means they've heard from us and can reply, let's assume public is active
                matchingNode->activatePublicSocket();
            }
        }
    } else {
        // let processNodeData handle it.
        NodeList::getInstance()->processNodeData(senderSockAddr, (unsigned char*) dataByteArray.data(), dataByteArray.size());
    }
}

void AudioMixer::run() {
    
    NodeList* nodeList = NodeList::getInstance();
    
    // change the logging target name while this is running
    Logging::setTargetName(AUDIO_MIXER_LOGGING_TARGET_NAME);
    
    nodeList->setOwnerType(NODE_TYPE_AUDIO_MIXER);
    
    const char AUDIO_MIXER_NODE_TYPES_OF_INTEREST[2] = { NODE_TYPE_AGENT, NODE_TYPE_AUDIO_INJECTOR };
    nodeList->setNodeTypesOfInterest(AUDIO_MIXER_NODE_TYPES_OF_INTEREST, sizeof(AUDIO_MIXER_NODE_TYPES_OF_INTEREST));
    
    nodeList->linkedDataCreateCallback = attachNewBufferToNode;
    
    QTimer* domainServerTimer = new QTimer(this);
    connect(domainServerTimer, SIGNAL(timeout()), this, SLOT(checkInWithDomainServerOrExit()));
    domainServerTimer->start(DOMAIN_SERVER_CHECK_IN_USECS / 1000);
    
    QTimer* silentNodeTimer = new QTimer(this);
    connect(silentNodeTimer, SIGNAL(timeout()), nodeList, SLOT(removeSilentNodes()));
    silentNodeTimer->start(NODE_SILENCE_THRESHOLD_USECS / 1000);
    
    QTimer* pingNodesTimer = new QTimer(this);
    connect(pingNodesTimer, SIGNAL(timeout()), nodeList, SLOT(pingInactiveNodes()));
    pingNodesTimer->start(PING_INACTIVE_NODE_INTERVAL_USECS / 1000);
    
    int nextFrame = 0;
    timeval startTime;
    
    gettimeofday(&startTime, NULL);
    
    int numBytesPacketHeader = numBytesForPacketHeader((unsigned char*) &PACKET_TYPE_MIXED_AUDIO);
    unsigned char clientPacket[BUFFER_LENGTH_BYTES_STEREO + numBytesPacketHeader];
    populateTypeAndVersion(clientPacket, PACKET_TYPE_MIXED_AUDIO);
    
    while (!_isFinished) {
        
        QCoreApplication::processEvents();
        
        if (_isFinished) {
            break;
        }

        for (NodeList::iterator node = nodeList->begin(); node != nodeList->end(); node++) {
            if (node->getLinkedData()) {
                ((AudioMixerClientData*) node->getLinkedData())->checkBuffersBeforeFrameSend(JITTER_BUFFER_SAMPLES);
            }
        }
        
        for (NodeList::iterator node = nodeList->begin(); node != nodeList->end(); node++) {
            if (node->getType() == NODE_TYPE_AGENT && node->getActiveSocket() && node->getLinkedData()
                && ((AudioMixerClientData*) node->getLinkedData())->getAvatarAudioRingBuffer()) {
                prepareMixForListeningNode(&(*node));
                
                memcpy(clientPacket + numBytesPacketHeader, _clientSamples, sizeof(_clientSamples));
                nodeList->getNodeSocket().writeDatagram((char*) clientPacket, sizeof(clientPacket),
                                                        node->getActiveSocket()->getAddress(),
                                                        node->getActiveSocket()->getPort());
            }
        }
        
        // push forward the next output pointers for any audio buffers we used
        for (NodeList::iterator node = nodeList->begin(); node != nodeList->end(); node++) {
            if (node->getLinkedData()) {
                ((AudioMixerClientData*) node->getLinkedData())->pushBuffersAfterFrameSend();
            }
        }
        
        int usecToSleep = usecTimestamp(&startTime) + (++nextFrame * BUFFER_SEND_INTERVAL_USECS) - usecTimestampNow();
        
        if (usecToSleep > 0) {
            usleep(usecToSleep);
        } else {
            qDebug("Took too much time, not sleeping!\n");
        }
        
    }
}