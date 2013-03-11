//
//  Audio.cpp
//  interface
//
//  Created by Stephen Birarda on 1/22/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <iostream>
#include <fstream>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <cstring>
#include "Audio.h"
#include "Util.h"
#include <SharedUtil.h>
#include "UDPSocket.h"

Oscilloscope * scope;

const int NUM_AUDIO_CHANNELS = 2;

const int PACKET_LENGTH_BYTES = 1024;
const int PACKET_LENGTH_BYTES_PER_CHANNEL = PACKET_LENGTH_BYTES / 2;
const int PACKET_LENGTH_SAMPLES = PACKET_LENGTH_BYTES / sizeof(int16_t);
const int PACKET_LENGTH_SAMPLES_PER_CHANNEL = PACKET_LENGTH_SAMPLES / 2;

const int BUFFER_LENGTH_BYTES = 512;
const int BUFFER_LENGTH_SAMPLES = BUFFER_LENGTH_BYTES / sizeof(int16_t);

const int RING_BUFFER_FRAMES = 10;
const int RING_BUFFER_SAMPLES = RING_BUFFER_FRAMES * BUFFER_LENGTH_SAMPLES;

const int PHASE_DELAY_AT_90 = 20;
const float AMPLITUDE_RATIO_AT_90 = 0.5;

const int SAMPLE_RATE = 22050;
const float JITTER_BUFFER_LENGTH_MSECS = 4;
const short JITTER_BUFFER_SAMPLES = JITTER_BUFFER_LENGTH_MSECS *
                                    NUM_AUDIO_CHANNELS * (SAMPLE_RATE / 1000.0);

const float AUDIO_CALLBACK_MSECS = (float)BUFFER_LENGTH_SAMPLES / (float)SAMPLE_RATE * 1000.0;


const int AGENT_LOOPBACK_MODIFIER = 307;

const char LOCALHOST_MIXER[] = "0.0.0.0";
const char WORKCLUB_MIXER[] = "192.168.1.19";
const char EC2_WEST_MIXER[] = "54.241.92.53";

const int AUDIO_UDP_LISTEN_PORT = 55444;

int starve_counter = 0;
StDev stdev;
bool stopAudioReceiveThread = false;

timeval firstPlaybackTimer;
int packetsReceivedThisPlayback = 0;

#define LOG_SAMPLE_DELAY 0

std::ofstream logFile;

/**
 * Audio callback used by portaudio.
 * Communicates with Audio via a shared pointer to Audio::data.
 * Writes input audio channels (if they exist) into Audio::data->buffer,
 multiplied by Audio::data->inputGain.
 * Then writes Audio::data->buffer into output audio channels, and clears
 the portion of Audio::data->buffer that has been read from for reuse.
 *
 * @param[in]  inputBuffer  A pointer to an internal portaudio data buffer containing data read by portaudio.
 * @param[out] outputBuffer A pointer to an internal portaudio data buffer to be read by the configured output device.
 * @param[in]  frames       Number of frames that portaudio requests to be read/written.
 (Valid size of input/output buffers = frames * number of channels (2) * sizeof data type (float)).
 * @param[in]  timeInfo     Portaudio time info. Currently unused.
 * @param[in]  statusFlags  Portaudio status flags. Currently unused.
 * @param[in]  userData     Pointer to supplied user data (in this case, a pointer to Audio::data).
 Used to communicate with external code (since portaudio calls this function from another thread).
 * @return Should be of type PaStreamCallbackResult. Return paComplete to end the stream, or paContinue to continue (default).
 Can be used to end the stream from within the callback.
 */

int audioCallback (const void *inputBuffer,
                   void *outputBuffer,
                   unsigned long frames,
                   const PaStreamCallbackTimeInfo *timeInfo,
                   PaStreamCallbackFlags statusFlags,
                   void *userData)
{
    AudioData *data = (AudioData *) userData;
    
    int16_t *inputLeft = ((int16_t **) inputBuffer)[0];
//    int16_t *inputRight = ((int16_t **) inputBuffer)[1];
    
    //printf("Audio callback at %6.0f\n", usecTimestampNow()/1000);
    
    if (inputLeft != NULL) {
        
        if (data->mixerAddress != 0) {
            sockaddr_in audioMixerSocket;
            audioMixerSocket.sin_family = AF_INET;
            audioMixerSocket.sin_addr.s_addr = data->mixerAddress;
            audioMixerSocket.sin_port = data->mixerPort;
            
            int leadingBytes = 1 + (sizeof(float) * 4);
            
            // we need the amount of bytes in the buffer + 1 for type + 12 for 3 floats for position
            unsigned char dataPacket[BUFFER_LENGTH_BYTES + leadingBytes];
            
            dataPacket[0] = 'I';
            unsigned char *currentPacketPtr = dataPacket + 1;
            
            // memcpy the three float positions
            for (int p = 0; p < 3; p++) {
                memcpy(currentPacketPtr, &data->linkedHead->getPos()[p], sizeof(float));
                currentPacketPtr += sizeof(float);
            }
            
            // memcpy the corrected render yaw
            float correctedYaw = fmodf(data->linkedHead->getRenderYaw(), 360);
            
            if (correctedYaw > 180) {
                correctedYaw -= 360;
            } else if (correctedYaw < -180) {
                correctedYaw += 360;
            }
            
            if (data->mixerLoopbackFlag) {
                correctedYaw = correctedYaw > 0 ? correctedYaw + AGENT_LOOPBACK_MODIFIER : correctedYaw - AGENT_LOOPBACK_MODIFIER;
            }
            
            memcpy(currentPacketPtr, &correctedYaw, sizeof(float));
            currentPacketPtr += sizeof(float);
                        
            // copy the audio data to the last BUFFER_LENGTH_BYTES bytes of the data packet
            memcpy(currentPacketPtr, inputLeft, BUFFER_LENGTH_BYTES);
            
            data->audioSocket->send((sockaddr *)&audioMixerSocket, dataPacket, BUFFER_LENGTH_BYTES + leadingBytes);
        }
       
        //
        //  Measure the loudness of the signal from the microphone and store in audio object
        //
        float loudness = 0;
        for (int i = 0; i < BUFFER_LENGTH_SAMPLES; i++) {
            loudness += abs(inputLeft[i]);
        }
        
        loudness /= BUFFER_LENGTH_SAMPLES;
        data->lastInputLoudness = loudness;
        data->averagedInputLoudness = 0.66*data->averagedInputLoudness + 0.33*loudness;
        //
        //  If scope is turned on, copy input buffer to scope
        //
        if (scope->getState()) {
            for (int i = 0; i < BUFFER_LENGTH_SAMPLES; i++) {
                scope->addData((float)inputLeft[i]/32767.0, 1, i);
            }
        }
    }
    
    int16_t *outputLeft = ((int16_t **) outputBuffer)[0];
    int16_t *outputRight = ((int16_t **) outputBuffer)[1];
    
    memset(outputLeft, 0, PACKET_LENGTH_BYTES_PER_CHANNEL);
    memset(outputRight, 0, PACKET_LENGTH_BYTES_PER_CHANNEL);

    AudioRingBuffer *ringBuffer = data->ringBuffer;
    
    // if we've been reset, and there isn't any new packets yet
    // just play some silence
    
    if (ringBuffer->getEndOfLastWrite() != NULL) {
        
        if (!ringBuffer->isStarted() && ringBuffer->diffLastWriteNextOutput() < PACKET_LENGTH_SAMPLES + JITTER_BUFFER_SAMPLES) {
            printf("Held back, buffer has %d of %d samples required.\n", ringBuffer->diffLastWriteNextOutput(), PACKET_LENGTH_SAMPLES + JITTER_BUFFER_SAMPLES);
        } else if (ringBuffer->diffLastWriteNextOutput() < PACKET_LENGTH_SAMPLES) {
            ringBuffer->setStarted(false);
            
            starve_counter++;
            packetsReceivedThisPlayback = 0;

            printf("Starved #%d\n", starve_counter);
            data->wasStarved = 10;      //   Frames to render the indication that the system was starved.
        } else {
            if (!ringBuffer->isStarted()) {
                ringBuffer->setStarted(true);
                printf("starting playback %3.1f msecs delayed, \n", (usecTimestampNow() - usecTimestamp(&firstPlaybackTimer))/1000.0);
            } else {
                //printf("pushing buffer\n");
            }
            // play whatever we have in the audio buffer
            
            memcpy(outputLeft, ringBuffer->getNextOutput(), PACKET_LENGTH_BYTES_PER_CHANNEL);
            memcpy(outputRight, ringBuffer->getNextOutput() + PACKET_LENGTH_SAMPLES_PER_CHANNEL, PACKET_LENGTH_BYTES_PER_CHANNEL);
            
            ringBuffer->setNextOutput(ringBuffer->getNextOutput() + PACKET_LENGTH_SAMPLES);
            
            if (ringBuffer->getNextOutput() == ringBuffer->getBuffer() + RING_BUFFER_SAMPLES) {
                ringBuffer->setNextOutput(ringBuffer->getBuffer());
            }
        }
    }
    
    gettimeofday(&data->lastCallback, NULL);
    return paContinue;
}

void Audio::updateMixerParams(in_addr_t newMixerAddress, in_port_t newMixerPort) {
    audioData->mixerAddress = newMixerAddress;
    audioData->mixerPort = newMixerPort;
}

struct AudioRecThreadStruct {
    AudioData *sharedAudioData;
};

void *receiveAudioViaUDP(void *args) {
    AudioRecThreadStruct *threadArgs = (AudioRecThreadStruct *) args;
    AudioData *sharedAudioData = threadArgs->sharedAudioData;
    
    int16_t *receivedData = new int16_t[PACKET_LENGTH_SAMPLES];
    ssize_t receivedBytes;
    
    timeval previousReceiveTime, currentReceiveTime = {};
    
    if (LOG_SAMPLE_DELAY) {
        gettimeofday(&previousReceiveTime, NULL);
        
        char *directory = new char[50];
        char *filename = new char[50];
        
        sprintf(directory, "%s/Desktop/echo_tests", getenv("HOME"));
        
        mkdir(directory, S_IRWXU | S_IRWXG | S_IRWXO);
        sprintf(filename, "%s/%ld.csv", directory, previousReceiveTime.tv_sec);
        
        logFile.open(filename, std::ios::out);
        
        delete[] directory;
        delete[] filename;
    }
        
    while (!stopAudioReceiveThread) {
        if (sharedAudioData->audioSocket->receive((void *)receivedData, &receivedBytes)) {
            bool firstSample = (currentReceiveTime.tv_sec == 0);
            
            gettimeofday(&currentReceiveTime, NULL);

            if (LOG_SAMPLE_DELAY) {
                if (!firstSample) {
                    // write time difference (in microseconds) between packet receipts to file
                    double timeDiff = diffclock(&previousReceiveTime, &currentReceiveTime);
                    logFile << timeDiff << std::endl;
                }
            }
            
            //  Compute and report standard deviation for jitter calculation
            if (firstSample) {
                stdev.reset();
            } else {
                double tDiff = diffclock(&previousReceiveTime, &currentReceiveTime);
                //printf(\n";
                stdev.addValue(tDiff);
                if (stdev.getSamples() > 500) {
                    sharedAudioData->measuredJitter = stdev.getStDev();
                    printf("Avg: %4.2f, Stdev: %4.2f\n", stdev.getAverage(), sharedAudioData->measuredJitter);
                    stdev.reset();
                }
            }
            
            AudioRingBuffer *ringBuffer = sharedAudioData->ringBuffer;
            
            
            if (!ringBuffer->isStarted()) {
                printf("Audio packet %d received at %6.0f\n", ++packetsReceivedThisPlayback, usecTimestampNow()/1000);
             }
            else {
                //printf("Audio packet received at %6.0f\n", usecTimestampNow()/1000);
            }
            
            if (packetsReceivedThisPlayback == 1) gettimeofday(&firstPlaybackTimer, NULL);

            ringBuffer->parseData(receivedData, PACKET_LENGTH_BYTES);
    
            if (LOG_SAMPLE_DELAY) {
                gettimeofday(&previousReceiveTime, NULL);
            }
        }
    }
    
    pthread_exit(0);
}

void Audio::setMixerLoopbackFlag(bool newMixerLoopbackFlag) {
    audioData->mixerLoopbackFlag = newMixerLoopbackFlag;
}

bool Audio::getMixerLoopbackFlag() {
    return audioData->mixerLoopbackFlag;
}

/**
 * Initialize portaudio and start an audio stream.
 * Should be called at the beginning of program exection.
 * @seealso Audio::terminate
 * @return  Returns true if successful or false if an error occurred.
Use Audio::getError() to retrieve the error code.
 */
Audio::Audio(Oscilloscope *s, Head *linkedHead)
{
    paError = Pa_Initialize();
    if (paError != paNoError) goto error;
    
    scope = s;
    
    audioData = new AudioData();
    
    audioData->linkedHead = linkedHead;
    
    // setup a UDPSocket
    audioData->audioSocket = new UDPSocket(AUDIO_UDP_LISTEN_PORT);
    audioData->ringBuffer = new AudioRingBuffer(RING_BUFFER_SAMPLES, PACKET_LENGTH_SAMPLES);
    
    AudioRecThreadStruct threadArgs;
    threadArgs.sharedAudioData = audioData;
    
    pthread_create(&audioReceiveThread, NULL, receiveAudioViaUDP, (void *) &threadArgs);
    
    paError = Pa_OpenDefaultStream(&stream,
                               2,       // input channels
                               2,       // output channels
                               (paInt16 | paNonInterleaved), // sample format
                               SAMPLE_RATE,   // sample rate (hz)
                               BUFFER_LENGTH_SAMPLES,     // frames per buffer
                               audioCallback, // callback function
                               (void *) audioData);  // user data to be passed to callback
    if (paError != paNoError) goto error;
    
    initialized = true;
    
    // start the stream now that sources are good to go
    Pa_StartStream(stream);
    if (paError != paNoError) goto error;
    
     
    return;
    
error:
    fprintf(stderr, "-- Failed to initialize portaudio --\n");
    fprintf(stderr, "PortAudio error (%d): %s\n", paError, Pa_GetErrorText(paError));
    initialized = false;
    delete[] audioData;
}


void Audio::getInputLoudness(float * lastLoudness, float * averageLoudness) {
    *lastLoudness = audioData->lastInputLoudness;
    *averageLoudness = audioData->averagedInputLoudness;
}

void Audio::render(int screenWidth, int screenHeight)
{
    if (initialized) {
        glBegin(GL_LINES);
        glColor3f(1,1,1);
        
        int startX = 50.0;
        int currentX = startX;
        int topY = screenHeight - 90;
        int bottomY = screenHeight - 50;
        float frameWidth = 50.0;
        float halfY = topY + ((bottomY - topY) / 2.0);
        
        // draw the lines for the base of the ring buffer
        
        glVertex2f(currentX, topY);
        glVertex2f(currentX, bottomY);
        
        for (int i = 0; i < RING_BUFFER_FRAMES; i++) {
            glVertex2f(currentX, halfY);
            glVertex2f(currentX + frameWidth, halfY);
            currentX += frameWidth;
            
            glVertex2f(currentX, topY);
            glVertex2f(currentX, bottomY);
        }
        glEnd();
        
        
        //  Show a bar with the amount of audio remaining in ring buffer beyond current playback
        float remainingBuffer = 0;
        timeval currentTime;
        gettimeofday(&currentTime, NULL);
        float timeLeftInCurrentBuffer = 0;
        if (audioData->lastCallback.tv_usec > 0) {
            timeLeftInCurrentBuffer = AUDIO_CALLBACK_MSECS - diffclock(&audioData->lastCallback, &currentTime);
        }
    
        //  /(1000.0*(float)BUFFER_LENGTH_SAMPLES/(float)SAMPLE_RATE) * frameWidth
        
        if (audioData->ringBuffer->getEndOfLastWrite() != NULL)
            remainingBuffer = audioData->ringBuffer->diffLastWriteNextOutput() / PACKET_LENGTH_SAMPLES * AUDIO_CALLBACK_MSECS;
        
        if (audioData->wasStarved == 0) glColor3f(0, 1, 0);
        else {
            glColor3f(0.5 + (float)audioData->wasStarved/20.0, 0, 0);
            audioData->wasStarved--;
        }
        
        glBegin(GL_QUADS);
        glVertex2f(startX, topY + 5);
        glVertex2f(startX + (remainingBuffer + timeLeftInCurrentBuffer)/AUDIO_CALLBACK_MSECS*frameWidth, topY + 5);
        glVertex2f(startX + (remainingBuffer + timeLeftInCurrentBuffer)/AUDIO_CALLBACK_MSECS*frameWidth, bottomY - 5);
        glVertex2f(startX, bottomY - 5);
        glEnd();
        
        if (audioData->averagedLatency == 0.0) audioData->averagedLatency = remainingBuffer + timeLeftInCurrentBuffer;
        else audioData->averagedLatency = 0.99*audioData->averagedLatency + 0.01*((float)remainingBuffer + (float)timeLeftInCurrentBuffer);
        
        //  Show a yellow bar with the averaged msecs latency you are hearing (from time of packet receipt)
        glColor3f(1,1,0);
        glBegin(GL_QUADS);
        glVertex2f(startX + audioData->averagedLatency/AUDIO_CALLBACK_MSECS*frameWidth - 2, topY - 2);
        glVertex2f(startX + audioData->averagedLatency/AUDIO_CALLBACK_MSECS*frameWidth + 2, topY - 2);
        glVertex2f(startX + audioData->averagedLatency/AUDIO_CALLBACK_MSECS*frameWidth + 2, bottomY + 2);
        glVertex2f(startX + audioData->averagedLatency/AUDIO_CALLBACK_MSECS*frameWidth - 2, bottomY + 2);
        glEnd();
        
        char out[40];
        sprintf(out, "%3.0f\n", audioData->averagedLatency);
        drawtext(startX + audioData->averagedLatency/AUDIO_CALLBACK_MSECS*frameWidth - 10, topY-10, 0.08, 0, 1, 0, out, 1,1,0);
        //drawtext(startX + 0, topY-10, 0.08, 0, 1, 0, out, 1,1,0);
        
        //  Show a Cyan bar with the most recently measured jitter stdev
        
        int jitterPels = (float) audioData->measuredJitter/ ((1000.0*(float)PACKET_LENGTH_SAMPLES/(float)SAMPLE_RATE)) * (float)frameWidth;
        
        glColor3f(0,1,1);
        glBegin(GL_QUADS);
        glVertex2f(startX + jitterPels - 2, topY - 2);
        glVertex2f(startX + jitterPels + 2, topY - 2);
        glVertex2f(startX + jitterPels + 2, bottomY + 2);
        glVertex2f(startX + jitterPels - 2, bottomY + 2);
        glEnd();
        
        sprintf(out,"%3.1f\n", audioData->measuredJitter);
        drawtext(startX + jitterPels - 5, topY-10, 0.08, 0, 1, 0, out, 0,1,1);
        
        sprintf(out, "%3.1fms\n", JITTER_BUFFER_LENGTH_MSECS);
        drawtext(startX - 10, bottomY + 20, 0.1, 0, 1, 0, out, 1, 0, 0);
        
        sprintf(out, "%hd samples\n", JITTER_BUFFER_SAMPLES);
        drawtext(startX - 10, bottomY + 35, 0.1, 0, 1, 0, out, 1, 0, 0);
    }
}

/**
 * Close the running audio stream, and deinitialize portaudio.
 * Should be called at the end of program execution.
 * @return Returns true if the initialization was successful, or false if an error occured.
 The error code may be retrieved by Audio::getError().
 */
bool Audio::terminate ()
{
    stopAudioReceiveThread = true;
    pthread_join(audioReceiveThread, NULL);
    
    if (initialized) {
        initialized = false;
        
        paError = Pa_CloseStream(stream);
        if (paError != paNoError) goto error;
        
        paError = Pa_Terminate();
        if (paError != paNoError) goto error;
    }
    
    logFile.close();    
    delete audioData;
    
    return true;
    
error:
    fprintf(stderr, "-- portaudio termination error --\n");
    fprintf(stderr, "PortAudio error (%d): %s\n", paError, Pa_GetErrorText(paError));
    return false;
}

