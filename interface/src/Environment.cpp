//
//  Environment.cpp
//  interface
//
//  Created by Andrzej Kapolka on 5/6/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.

#include <QByteArray>
#include <QMutexLocker>
#include <QtDebug>

#include <GeometryUtil.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>

#include "Camera.h"
#include "Environment.h"
#include "renderer/ProgramObject.h"
#include "world.h"

uint qHash(const HifiSockAddr& sockAddr) {
    if (sockAddr.getAddress().isNull()) {
        return 0; // shouldn't happen, but if it does, zero is a perfectly valid hash
    }
    quint32 address = sockAddr.getAddress().toIPv4Address();
    return sockAddr.getPort() + qHash(QByteArray::fromRawData((char*) &address,
                                                              sizeof(address)));
}

Environment::Environment()
    : _initialized(false) {
}

Environment::~Environment() {
    if (_initialized) {
        delete _skyFromAtmosphereProgram;
        delete _skyFromSpaceProgram;
    }
}

void Environment::init() {
    if (_initialized) {
        qDebug("[ERROR] Environment is already initialized.\n");
        return;
    }

    switchToResourcesParentIfRequired();
    _skyFromAtmosphereProgram = createSkyProgram("Atmosphere", _skyFromAtmosphereUniformLocations);
    _skyFromSpaceProgram = createSkyProgram("Space", _skyFromSpaceUniformLocations);
    
    // start off with a default-constructed environment data
    _data[HifiSockAddr()][0];

    _initialized = true;
}

void Environment::resetToDefault() {
    _data.clear();
    _data[HifiSockAddr()][0];
}

void Environment::renderAtmospheres(Camera& camera) {    
    // get the lock for the duration of the call
    QMutexLocker locker(&_mutex);
    
    foreach (const ServerData& serverData, _data) {
        foreach (const EnvironmentData& environmentData, serverData) {
            renderAtmosphere(camera, environmentData);
        }
    }
}

glm::vec3 Environment::getGravity (const glm::vec3& position) {
    //
    // 'Default' gravity pulls you downward in Y when you are near the X/Z plane
    const glm::vec3 DEFAULT_GRAVITY(0.f, -1.f, 0.f);
    glm::vec3 gravity(DEFAULT_GRAVITY);
    float DEFAULT_SURFACE_RADIUS = 30.f;
    float gravityStrength;
    
    //  Weaken gravity with height
    if (position.y > 0.f) {
        gravityStrength = 1.f / powf((DEFAULT_SURFACE_RADIUS + position.y) / DEFAULT_SURFACE_RADIUS, 2.f);
        gravity *= gravityStrength;
    }
    
    // get the lock for the duration of the call
    QMutexLocker locker(&_mutex);
    
    foreach (const ServerData& serverData, _data) {
        foreach (const EnvironmentData& environmentData, serverData) {
            glm::vec3 vector = environmentData.getAtmosphereCenter() - position;
            float surfaceRadius = environmentData.getAtmosphereInnerRadius();
            if (glm::length(vector) <= surfaceRadius) {
                //  At or inside a planet, gravity is as set for the planet
                gravity += glm::normalize(vector) * environmentData.getGravity();
            } else {
                //  Outside a planet, the gravity falls off with distance 
                gravityStrength = 1.f / powf(glm::length(vector) / surfaceRadius, 2.f);
                gravity += glm::normalize(vector) * environmentData.getGravity() * gravityStrength;
            }
        }
    }
    
    return gravity;
}

const EnvironmentData Environment::getClosestData(const glm::vec3& position) {
    // get the lock for the duration of the call
    QMutexLocker locker(&_mutex);
    
    EnvironmentData closest;
    float closestDistance = FLT_MAX;
    foreach (const ServerData& serverData, _data) {
        foreach (const EnvironmentData& environmentData, serverData) {
            float distance = glm::distance(position, environmentData.getAtmosphereCenter()) -
                environmentData.getAtmosphereOuterRadius();
            if (distance < closestDistance) {
                closest = environmentData;
                closestDistance = distance;
            }
        }
    }
    return closest;
}

bool Environment::findCapsulePenetration(const glm::vec3& start, const glm::vec3& end,
                                         float radius, glm::vec3& penetration) {
    // collide with the "floor"
    bool found = findCapsulePlanePenetration(start, end, radius, glm::vec4(0.0f, 1.0f, 0.0f, 0.0f), penetration);
    
    // get the lock for the duration of the call
    QMutexLocker locker(&_mutex);
    
    foreach (const ServerData& serverData, _data) {
        foreach (const EnvironmentData& environmentData, serverData) {
            if (environmentData.getGravity() == 0.0f) {
                continue; // don't bother colliding with gravity-less environments
            }
            glm::vec3 environmentPenetration;
            if (findCapsuleSpherePenetration(start, end, radius, environmentData.getAtmosphereCenter(),
                    environmentData.getAtmosphereInnerRadius(), environmentPenetration)) {
                penetration = addPenetrations(penetration, environmentPenetration);
                found = true;
            }
        }
    }
    return found;
}

int Environment::parseData(const HifiSockAddr& senderAddress, unsigned char* sourceBuffer, int numBytes) {
    // push past the packet header
    unsigned char* start = sourceBuffer;
    
    int numBytesPacketHeader = numBytesForPacketHeader(sourceBuffer);
    sourceBuffer += numBytesPacketHeader;
    numBytes -= numBytesPacketHeader;
    
    // get the lock for the duration of the call
    QMutexLocker locker(&_mutex);
    
    EnvironmentData newData;
    while (numBytes > 0) {
        int dataLength = newData.parseData(sourceBuffer, numBytes);
        
        // update the mapping by address/ID
        _data[senderAddress][newData.getID()] = newData;
        
        sourceBuffer += dataLength;
        numBytes -= dataLength;    
    }
    
    // remove the default mapping, if any
    _data.remove(HifiSockAddr());
    
    return sourceBuffer - start;
}

ProgramObject* Environment::createSkyProgram(const char* from, int* locations) {
    ProgramObject* program = new ProgramObject();
    QByteArray prefix = QByteArray("resources/shaders/SkyFrom") + from;
    program->addShaderFromSourceFile(QGLShader::Vertex, prefix + ".vert");
    program->addShaderFromSourceFile(QGLShader::Fragment, prefix + ".frag");
    program->link();
    
    locations[CAMERA_POS_LOCATION] = program->uniformLocation("v3CameraPos");
    locations[LIGHT_POS_LOCATION] = program->uniformLocation("v3LightPos");
    locations[INV_WAVELENGTH_LOCATION] = program->uniformLocation("v3InvWavelength");
    locations[CAMERA_HEIGHT2_LOCATION] = program->uniformLocation("fCameraHeight2");
    locations[OUTER_RADIUS_LOCATION] = program->uniformLocation("fOuterRadius");
    locations[OUTER_RADIUS2_LOCATION] = program->uniformLocation("fOuterRadius2");
    locations[INNER_RADIUS_LOCATION] = program->uniformLocation("fInnerRadius");
    locations[KR_ESUN_LOCATION] = program->uniformLocation("fKrESun");
    locations[KM_ESUN_LOCATION] = program->uniformLocation("fKmESun");
    locations[KR_4PI_LOCATION] = program->uniformLocation("fKr4PI");
    locations[KM_4PI_LOCATION] = program->uniformLocation("fKm4PI");
    locations[SCALE_LOCATION] = program->uniformLocation("fScale");
    locations[SCALE_DEPTH_LOCATION] = program->uniformLocation("fScaleDepth");
    locations[SCALE_OVER_SCALE_DEPTH_LOCATION] = program->uniformLocation("fScaleOverScaleDepth");
    locations[G_LOCATION] = program->uniformLocation("g");
    locations[G2_LOCATION] = program->uniformLocation("g2");
    
    return program;
}

void Environment::renderAtmosphere(Camera& camera, const EnvironmentData& data) {
    glPushMatrix();
    glTranslatef(data.getAtmosphereCenter().x, data.getAtmosphereCenter().y, data.getAtmosphereCenter().z);

    glm::vec3 relativeCameraPos = camera.getPosition() - data.getAtmosphereCenter();
    float height = glm::length(relativeCameraPos);
    
    // use the appropriate shader depending on whether we're inside or outside
    ProgramObject* program;
    int* locations;
    if (height < data.getAtmosphereOuterRadius()) {
        program = _skyFromAtmosphereProgram;
        locations = _skyFromAtmosphereUniformLocations;
        
    } else {
        program = _skyFromSpaceProgram;
        locations = _skyFromSpaceUniformLocations;
    }
    
    // the constants here are from Sean O'Neil's GPU Gems entry
    // (http://http.developer.nvidia.com/GPUGems2/gpugems2_chapter16.html), GameEngine.cpp
    program->bind();
    program->setUniform(locations[CAMERA_POS_LOCATION], relativeCameraPos);
    glm::vec3 lightDirection = glm::normalize(data.getSunLocation());
    program->setUniform(locations[LIGHT_POS_LOCATION], lightDirection);
    program->setUniformValue(locations[INV_WAVELENGTH_LOCATION],
        1 / powf(data.getScatteringWavelengths().r, 4.0f),
        1 / powf(data.getScatteringWavelengths().g, 4.0f),
        1 / powf(data.getScatteringWavelengths().b, 4.0f));
    program->setUniformValue(locations[CAMERA_HEIGHT2_LOCATION], height * height);
    program->setUniformValue(locations[OUTER_RADIUS_LOCATION], data.getAtmosphereOuterRadius());
    program->setUniformValue(locations[OUTER_RADIUS2_LOCATION], data.getAtmosphereOuterRadius() * data.getAtmosphereOuterRadius());
    program->setUniformValue(locations[INNER_RADIUS_LOCATION], data.getAtmosphereInnerRadius());
    program->setUniformValue(locations[KR_ESUN_LOCATION], data.getRayleighScattering() * data.getSunBrightness());
    program->setUniformValue(locations[KM_ESUN_LOCATION], data.getMieScattering() * data.getSunBrightness());
    program->setUniformValue(locations[KR_4PI_LOCATION], data.getRayleighScattering() * 4.0f * PIf);
    program->setUniformValue(locations[KM_4PI_LOCATION], data.getMieScattering() * 4.0f * PIf);
    program->setUniformValue(locations[SCALE_LOCATION], 1.0f / (data.getAtmosphereOuterRadius() - data.getAtmosphereInnerRadius()));
    program->setUniformValue(locations[SCALE_DEPTH_LOCATION], 0.25f);
    program->setUniformValue(locations[SCALE_OVER_SCALE_DEPTH_LOCATION],
        (1.0f / (data.getAtmosphereOuterRadius() - data.getAtmosphereInnerRadius())) / 0.25f);
    program->setUniformValue(locations[G_LOCATION], -0.990f);
    program->setUniformValue(locations[G2_LOCATION], -0.990f * -0.990f);
    
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glutSolidSphere(data.getAtmosphereOuterRadius(), 100, 50);
    glDepthMask(GL_TRUE);
    
    program->release();
    
    glPopMatrix();  
}
