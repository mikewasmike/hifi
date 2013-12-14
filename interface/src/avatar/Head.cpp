//
//  Head.cpp
//  interface
//
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.

#include <glm/gtx/quaternion.hpp>

#include <NodeList.h>

#include "Application.h"
#include "Menu.h"
#include "Avatar.h"
#include "Head.h"
#include "Util.h"
#include "renderer/ProgramObject.h"

using namespace std;

const bool  USING_PHYSICAL_MOHAWK    =  true;
const float EYE_RIGHT_OFFSET         =  0.27f;
const float EYE_UP_OFFSET            =  0.36f;
const float EYE_FRONT_OFFSET         =  0.8f;
const float EAR_RIGHT_OFFSET         =  1.0;
const float MOUTH_UP_OFFSET          = -0.3f;
const float HEAD_MOTION_DECAY        =  0.1;
const float MINIMUM_EYE_ROTATION_DOT =  0.5f; // based on a dot product: 1.0 is straight ahead, 0.0 is 90 degrees off
const float EYEBALL_RADIUS           =  0.017;
const float EYELID_RADIUS            =  0.019; 
const float EYEBALL_COLOR[3]         =  { 0.9f, 0.9f, 0.8f };

const float HAIR_SPRING_FORCE        =  15.0f; 
const float HAIR_TORQUE_FORCE        =  0.2f;
const float HAIR_GRAVITY_FORCE       =  0.001f;
const float HAIR_DRAG                =  10.0f;

const float HAIR_LENGTH              =  0.09f;
const float HAIR_THICKNESS           =  0.03f;
const float NOSE_LENGTH              =  0.025;
const float NOSE_WIDTH               =  0.03;
const float NOSE_HEIGHT              =  0.034;
const float NOSE_UP_OFFSET           = -0.07;
const float NOSE_UPTURN              =  0.005;
const float IRIS_RADIUS              =  0.007;
const float IRIS_PROTRUSION          =  0.0145f;
const char  IRIS_TEXTURE_FILENAME[]  =  "resources/images/iris.png";

ProgramObject Head::_irisProgram;
QSharedPointer<DilatableNetworkTexture> Head::_irisTexture;
int Head::_eyePositionLocation;

Head::Head(Avatar* owningAvatar) :
    HeadData((AvatarData*)owningAvatar),
    yawRate(0.0f),
    _renderAlpha(0.0),
    _returnHeadToCenter(false),
    _skinColor(0.0f, 0.0f, 0.0f),
    _position(0.0f, 0.0f, 0.0f),
    _rotation(0.0f, 0.0f, 0.0f),
    _leftEyePosition(0.0f, 0.0f, 0.0f),
    _rightEyePosition(0.0f, 0.0f, 0.0f),
    _eyePosition(0.0f, 0.0f, 0.0f),
    _leftEyeBrowPosition(0.0f, 0.0f, 0.0f),
    _rightEyeBrowPosition(0.0f, 0.0f, 0.0f),
    _leftEarPosition(0.0f, 0.0f, 0.0f),
    _rightEarPosition(0.0f, 0.0f, 0.0f),
    _mouthPosition(0.0f, 0.0f, 0.0f),
    _scale(1.0f),
    _gravity(0.0f, -1.0f, 0.0f),
    _lastLoudness(0.0f),
    _audioAttack(0.0f),
    _returnSpringScale(1.0f),
    _bodyRotation(0.0f, 0.0f, 0.0f),
    _angularVelocity(0,0,0),
    _renderLookatVectors(false),
    _mohawkInitialized(false),
    _saccade(0.0f, 0.0f, 0.0f),
    _saccadeTarget(0.0f, 0.0f, 0.0f),
    _leftEyeBlinkVelocity(0.0f),
    _rightEyeBlinkVelocity(0.0f),
    _timeWithoutTalking(0.0f),
    _cameraPitch(_pitch),
    _mousePitch(0.f),
    _cameraYaw(_yaw),
    _isCameraMoving(false),
    _videoFace(this),
    _faceModel(this)
{
    if (USING_PHYSICAL_MOHAWK) {    
        resetHairPhysics();
    }
}

void Head::init() {
    if (!_irisProgram.isLinked()) {
        switchToResourcesParentIfRequired();
        _irisProgram.addShaderFromSourceFile(QGLShader::Vertex, "resources/shaders/iris.vert");
        _irisProgram.addShaderFromSourceFile(QGLShader::Fragment, "resources/shaders/iris.frag");
        _irisProgram.link();

        _irisProgram.setUniformValue("texture", 0);
        _eyePositionLocation = _irisProgram.uniformLocation("eyePosition");
        
        _irisTexture = Application::getInstance()->getTextureCache()->getTexture(QUrl::fromLocalFile(IRIS_TEXTURE_FILENAME),
            false, true).staticCast<DilatableNetworkTexture>();
    }
    _faceModel.init();
}

void Head::reset() {
    _yaw = _pitch = _roll = 0.0f;
    _mousePitch = 0.0f;
    _leanForward = _leanSideways = 0.0f;
    
    if (USING_PHYSICAL_MOHAWK) {
        resetHairPhysics();
    }
    
    _faceModel.reset();
}

void Head::resetHairPhysics() {
    for (int t = 0; t < NUM_HAIR_TUFTS; t ++) {
        for (int t = 0; t < NUM_HAIR_TUFTS; t ++) {

            _hairTuft[t].setSpringForce  (HAIR_SPRING_FORCE);
            _hairTuft[t].setTorqueForce  (HAIR_TORQUE_FORCE);
            _hairTuft[t].setGravityForce (HAIR_GRAVITY_FORCE * _gravity);
            _hairTuft[t].setDrag         (HAIR_DRAG);
            _hairTuft[t].setLength       (_scale * HAIR_LENGTH   );
            _hairTuft[t].setThickness    (_scale * HAIR_THICKNESS);
            _hairTuft[t].setBaseDirection(getUpDirection());
            _hairTuft[t].reset();
        }
    }
}


void Head::simulate(float deltaTime, bool isMine) {
    
    //  Update audio trailing average for rendering facial animations
    Faceshift* faceshift = Application::getInstance()->getFaceshift();
    if (isMine) {
        _isFaceshiftConnected = faceshift->isActive();
    }
    
    if (isMine && faceshift->isActive()) {
        const float EYE_OPEN_SCALE = 0.5f;
        _leftEyeBlink = faceshift->getLeftBlink() - EYE_OPEN_SCALE * faceshift->getLeftEyeOpen();
        _rightEyeBlink = faceshift->getRightBlink() - EYE_OPEN_SCALE * faceshift->getRightEyeOpen();
        
        // set these values based on how they'll be used.  if we use faceshift in the long term, we'll want a complete
        // mapping between their blendshape coefficients and our avatar features
        const float MOUTH_SIZE_SCALE = 2500.0f;
        _averageLoudness = faceshift->getMouthSize() * faceshift->getMouthSize() * MOUTH_SIZE_SCALE;
        const float BROW_HEIGHT_SCALE = 0.005f;
        _browAudioLift = faceshift->getBrowUpCenter() * BROW_HEIGHT_SCALE;
        _blendshapeCoefficients = faceshift->getBlendshapeCoefficients();
        
    } else if (!_isFaceshiftConnected) {
        // Update eye saccades
        const float AVERAGE_MICROSACCADE_INTERVAL = 0.50f;
        const float AVERAGE_SACCADE_INTERVAL = 4.0f;
        const float MICROSACCADE_MAGNITUDE = 0.002f;
        const float SACCADE_MAGNITUDE = 0.04f;
        
        if (randFloat() < deltaTime / AVERAGE_MICROSACCADE_INTERVAL) {
            _saccadeTarget = MICROSACCADE_MAGNITUDE * randVector();
        } else if (randFloat() < deltaTime / AVERAGE_SACCADE_INTERVAL) {
            _saccadeTarget = SACCADE_MAGNITUDE * randVector();
        }
        _saccade += (_saccadeTarget - _saccade) * 0.50f;
    
        const float AUDIO_AVERAGING_SECS = 0.05;
        _averageLoudness = (1.f - deltaTime / AUDIO_AVERAGING_SECS) * _averageLoudness +
                                 (deltaTime / AUDIO_AVERAGING_SECS) * _audioLoudness;
        
        //  Detect transition from talking to not; force blink after that and a delay
        bool forceBlink = false;
        const float TALKING_LOUDNESS = 100.0f;
        const float BLINK_AFTER_TALKING = 0.25f;
        if (_averageLoudness > TALKING_LOUDNESS) {
            _timeWithoutTalking = 0.0f;
        
        } else if (_timeWithoutTalking < BLINK_AFTER_TALKING && (_timeWithoutTalking += deltaTime) >= BLINK_AFTER_TALKING) {
            forceBlink = true;
        }
                                 
        //  Update audio attack data for facial animation (eyebrows and mouth)
        _audioAttack = 0.9f * _audioAttack + 0.1f * fabs(_audioLoudness - _lastLoudness);
        _lastLoudness = _audioLoudness;
        
        const float BROW_LIFT_THRESHOLD = 100.0f;
        if (_audioAttack > BROW_LIFT_THRESHOLD) {
            _browAudioLift += sqrtf(_audioAttack) * 0.00005f;
        }
        
        const float CLAMP = 0.01f;
        if (_browAudioLift > CLAMP) {
            _browAudioLift = CLAMP;
        }
        
        _browAudioLift *= 0.7f;      

        const float BLINK_SPEED = 10.0f;
        const float FULLY_OPEN = 0.0f;
        const float FULLY_CLOSED = 1.0f;
        if (_leftEyeBlinkVelocity == 0.0f && _rightEyeBlinkVelocity == 0.0f) {
            // no blinking when brows are raised; blink less with increasing loudness
            const float BASE_BLINK_RATE = 15.0f / 60.0f;
            const float ROOT_LOUDNESS_TO_BLINK_INTERVAL = 0.25f;
            if (forceBlink || (_browAudioLift < EPSILON && shouldDo(glm::max(1.0f, sqrt(_averageLoudness) *
                    ROOT_LOUDNESS_TO_BLINK_INTERVAL) / BASE_BLINK_RATE, deltaTime))) {
                _leftEyeBlinkVelocity = BLINK_SPEED;
                _rightEyeBlinkVelocity = BLINK_SPEED;
            }
        } else {
            _leftEyeBlink = glm::clamp(_leftEyeBlink + _leftEyeBlinkVelocity * deltaTime, FULLY_OPEN, FULLY_CLOSED);
            _rightEyeBlink = glm::clamp(_rightEyeBlink + _rightEyeBlinkVelocity * deltaTime, FULLY_OPEN, FULLY_CLOSED);
            
            if (_leftEyeBlink == FULLY_CLOSED) {
                _leftEyeBlinkVelocity = -BLINK_SPEED;
            
            } else if (_leftEyeBlink == FULLY_OPEN) {
                _leftEyeBlinkVelocity = 0.0f;
            }
            if (_rightEyeBlink == FULLY_CLOSED) {
                _rightEyeBlinkVelocity = -BLINK_SPEED;
            
            } else if (_rightEyeBlink == FULLY_OPEN) {
                _rightEyeBlinkVelocity = 0.0f;
            }
        }
        
        // use data to update fake Faceshift blendshape coefficients
        const float BROW_LIFT_SCALE = 500.0f;
        const float JAW_OPEN_SCALE = 0.01f;
        const float JAW_OPEN_DEAD_ZONE = 0.75f;
        faceshift->updateFakeCoefficients(_leftEyeBlink, _rightEyeBlink, min(1.0f, _browAudioLift * BROW_LIFT_SCALE),
            glm::clamp(sqrt(_averageLoudness * JAW_OPEN_SCALE) - JAW_OPEN_DEAD_ZONE, 0.0f, 1.0f), _blendshapeCoefficients);
    }
    
    // based on the nature of the lookat position, determine if the eyes can look / are looking at it.      
    if (USING_PHYSICAL_MOHAWK) {
        updateHairPhysics(deltaTime);
    }
    
    _faceModel.simulate(deltaTime);
    
    calculateGeometry();
    
    // the blend face may have custom eye meshes
    _faceModel.getEyePositions(_leftEyePosition, _rightEyePosition);

}

void Head::calculateGeometry() {
    //generate orientation directions 
    glm::quat orientation = getOrientation();
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    glm::vec3 up    = orientation * IDENTITY_UP;
    glm::vec3 front = orientation * IDENTITY_FRONT;

    float scale = _scale * BODY_BALL_RADIUS_HEAD_BASE;

    //calculate the eye positions 
    _leftEyePosition  = _position 
                      - right * scale * EYE_RIGHT_OFFSET
                      + up    * scale * EYE_UP_OFFSET
                      + front * scale * EYE_FRONT_OFFSET;
    _rightEyePosition = _position
                      + right * scale * EYE_RIGHT_OFFSET 
                      + up    * scale * EYE_UP_OFFSET 
                      + front * scale * EYE_FRONT_OFFSET;

    _eyePosition = _rightEyePosition - right * scale * EYE_RIGHT_OFFSET;

    //calculate the eyebrow positions 
    _leftEyeBrowPosition  = _leftEyePosition; 
    _rightEyeBrowPosition = _rightEyePosition;
    
    //calculate the ear positions 
    _leftEarPosition  = _position - right * scale * EAR_RIGHT_OFFSET;
    _rightEarPosition = _position + right * scale * EAR_RIGHT_OFFSET;

    //calculate the mouth position 
    _mouthPosition = _position + up * scale * MOUTH_UP_OFFSET + front * scale;

    // calculate nose geometry
    glm::vec3 noseBase = _position + front * 0.95f * scale + up * NOSE_UP_OFFSET * scale;
    
    _nose.top   = noseBase + up    * _scale * NOSE_HEIGHT;
    _nose.left  = noseBase - right * _scale * NOSE_WIDTH * ONE_HALF;
    _nose.right = noseBase + right * _scale * NOSE_WIDTH * ONE_HALF;
    _nose.front = noseBase + front * _scale * NOSE_LENGTH 
                           + up    * _scale * NOSE_UPTURN;  
}

void Head::render(float alpha, bool renderAvatarBalls) {
    _renderAlpha = alpha;

    bool lookatVectorsVisible = _renderLookatVectors;
    if (renderAvatarBalls) {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_RESCALE_NORMAL);
        
        renderMohawk();
        renderHeadSphere();
        renderEyeBalls();
        renderEars();
        renderMouth();
        renderNose();
        renderEyeBrows();
    
    } else if (!_videoFace.render(alpha)) {
        lookatVectorsVisible &= _faceModel.render(alpha);
    }
    
    if (lookatVectorsVisible) {
        renderLookatVectors(_leftEyePosition, _rightEyePosition, _lookAtPosition);
    }
}

void Head::setScale (float scale) {
    if (_scale == scale) {
        return;
    }
    _scale = scale;

    createMohawk();

    if (USING_PHYSICAL_MOHAWK) {
        for (int t = 0; t < NUM_HAIR_TUFTS; t ++) {
    
            _hairTuft[t].setLength   (_scale * HAIR_LENGTH   );
            _hairTuft[t].setThickness(_scale * HAIR_THICKNESS);
        }
    }
}

void Head::setMousePitch(float mousePitch) {
    const float MAX_PITCH = 90.0f;
    _mousePitch = glm::clamp(mousePitch, -MAX_PITCH, MAX_PITCH);
}

void Head::createMohawk() {
    srand(time(NULL));
    float height = _scale * (0.08f + randFloat() * 0.05f);
    float variance = 0.03 + randFloat() * 0.03f;
    const float RAD_PER_TRIANGLE = (2.3f + randFloat() * 0.2f) / (float)MOHAWK_TRIANGLES;
    _mohawkTriangleFan[0] = glm::vec3(0, 0, 0);
    glm::vec3 basicColor(randFloat(), randFloat(), randFloat());
    _mohawkColors[0] = basicColor;
    
    for (int i = 1; i < MOHAWK_TRIANGLES; i++) {
        _mohawkTriangleFan[i]  = glm::vec3((randFloat() - 0.5f) * variance,
                                           height * cosf(i * RAD_PER_TRIANGLE - PIf / 2.f)
                                           + (randFloat()  - 0.5f) * variance,
                                           height * sinf(i * RAD_PER_TRIANGLE - PIf / 2.f)
                                           + (randFloat() - 0.5f) * variance);
        _mohawkColors[i] = randFloat() * basicColor;

    }
}

void Head::renderMohawk() {
    
    if (!_mohawkInitialized) {
        createMohawk();
        _mohawkInitialized = true;
    }
    
    if (USING_PHYSICAL_MOHAWK) {
        for (int t = 0; t < NUM_HAIR_TUFTS; t ++) {

            glm::vec3 baseAxis   = _hairTuft[t].getMidPosition() - _hairTuft[t].getBasePosition();
            glm::vec3 midAxis    = _hairTuft[t].getEndPosition() - _hairTuft[t].getMidPosition();
            glm::vec3 viewVector = _hairTuft[t].getBasePosition() - Application::getInstance()->getCamera()->getPosition();
            
            glm::vec3 basePerpendicular = glm::normalize(glm::cross(baseAxis, viewVector));
            glm::vec3 midPerpendicular  = glm::normalize(glm::cross(midAxis,  viewVector));

            glm::vec3 base1 = _hairTuft[t].getBasePosition() - basePerpendicular * _hairTuft[t].getThickness() * ONE_HALF;
            glm::vec3 base2 = _hairTuft[t].getBasePosition() + basePerpendicular * _hairTuft[t].getThickness() * ONE_HALF;
            glm::vec3 mid1  = _hairTuft[t].getMidPosition()  - midPerpendicular  * _hairTuft[t].getThickness() * ONE_HALF * ONE_HALF;
            glm::vec3 mid2  = _hairTuft[t].getMidPosition()  + midPerpendicular  * _hairTuft[t].getThickness() * ONE_HALF * ONE_HALF;
            
            glColor3f(_mohawkColors[t].x, _mohawkColors[t].y, _mohawkColors[t].z);

            glBegin(GL_TRIANGLES);             
            glVertex3f(base1.x,  base1.y,  base1.z ); 
            glVertex3f(base2.x,  base2.y,  base2.z ); 
            glVertex3f(mid1.x,   mid1.y,   mid1.z  ); 
            glVertex3f(base2.x,  base2.y,  base2.z ); 
            glVertex3f(mid1.x,   mid1.y,   mid1.z  ); 
            glVertex3f(mid2.x,   mid2.y,   mid2.z  ); 
            glVertex3f(mid1.x,   mid1.y,   mid1.z  ); 
            glVertex3f(mid2.x,   mid2.y,   mid2.z  ); 
            glVertex3f(_hairTuft[t].getEndPosition().x, _hairTuft[t].getEndPosition().y, _hairTuft[t].getEndPosition().z  ); 
            glEnd();
        }
    } else {
        glPushMatrix();
        glTranslatef(_position.x, _position.y, _position.z);
        glRotatef(_bodyRotation.y + _yaw, 0, 1, 0);
        glRotatef(-_roll, 0, 0, 1);
        glRotatef(-_pitch - _bodyRotation.x, 1, 0, 0);
       
        glBegin(GL_TRIANGLE_FAN);
        for (int i = 0; i < MOHAWK_TRIANGLES; i++) {
            glColor3f(_mohawkColors[i].x, _mohawkColors[i].y, _mohawkColors[i].z);
            glVertex3fv(&_mohawkTriangleFan[i].x);
            glNormal3fv(&_mohawkColors[i].x);
        }
        glEnd();
        glPopMatrix();
    } 
}

glm::quat Head::getOrientation() const {
    return glm::quat(glm::radians(_bodyRotation)) * glm::quat(glm::radians(glm::vec3(_pitch, _yaw, _roll)));
}

glm::quat Head::getCameraOrientation () const {
    Avatar* owningAvatar = static_cast<Avatar*>(_owningAvatar);
    return owningAvatar->getWorldAlignedOrientation()
            * glm::quat(glm::radians(glm::vec3(_cameraPitch + _mousePitch, _cameraYaw, 0.0f)));
}

glm::quat Head::getEyeRotation(const glm::vec3& eyePosition) const {
    glm::quat orientation = getOrientation();
    return rotationBetween(orientation * IDENTITY_FRONT, _lookAtPosition + _saccade - eyePosition) * orientation;
}

glm::vec3 Head::getScalePivot() const {
    return _faceModel.isActive() ? _faceModel.getTranslation() : _position;
}

void Head::renderHeadSphere() {
    glPushMatrix();
        glTranslatef(_position.x, _position.y, _position.z); //translate to head position
        glScalef(_scale * BODY_BALL_RADIUS_HEAD_BASE,
                 _scale * BODY_BALL_RADIUS_HEAD_BASE,
                 _scale * BODY_BALL_RADIUS_HEAD_BASE); //scale to head size
        glColor4f(_skinColor.x, _skinColor.y, _skinColor.z, _renderAlpha);
        glutSolidSphere(1, 30, 30);
    glPopMatrix();
}

void Head::renderEars() {

    glPushMatrix();
        glColor4f(_skinColor.x, _skinColor.y, _skinColor.z, _renderAlpha);
        glTranslatef(_leftEarPosition.x, _leftEarPosition.y, _leftEarPosition.z);        
        glutSolidSphere(_scale * 0.02, 30, 30);
    glPopMatrix();

    glPushMatrix();
        glColor4f(_skinColor.x, _skinColor.y, _skinColor.z, _renderAlpha);
        glTranslatef(_rightEarPosition.x, _rightEarPosition.y, _rightEarPosition.z);        
        glutSolidSphere(_scale * 0.02, 30, 30);
    glPopMatrix();
}


void Head::renderNose() {

    glm::vec3 bridgeVector = _nose.front - _nose.top;
    glm::vec3 leftvector   = _nose.front - _nose.left;
    glm::vec3 rightvector  = _nose.front - _nose.right;
    
    glm::vec3 leftNormal  (glm::normalize(glm::cross(leftvector,   bridgeVector)));
    glm::vec3 rightNormal (glm::normalize(glm::cross(bridgeVector, rightvector )));
    glm::vec3 bottomNormal(glm::normalize(glm::cross(rightvector,  leftvector  )));
    
    glColor4f(_skinColor.x, _skinColor.y, _skinColor.z, _renderAlpha);    
    
    glBegin(GL_TRIANGLES);          
       
    glNormal3f(leftNormal.x, leftNormal.y, leftNormal.z);
    glVertex3f(_nose.top.x,   _nose.top.y,   _nose.top.z   ); 
    glVertex3f(_nose.left.x,  _nose.left.y,  _nose.left.z  ); 
    glVertex3f(_nose.front.x, _nose.front.y, _nose.front.z ); 

    glNormal3f(rightNormal.x, rightNormal.y, rightNormal.z);
    glVertex3f(_nose.top.x,   _nose.top.y,   _nose.top.z   ); 
    glVertex3f(_nose.right.x, _nose.right.y, _nose.right.z ); 
    glVertex3f(_nose.front.x, _nose.front.y, _nose.front.z ); 
    
    glNormal3f(bottomNormal.x, bottomNormal.y, bottomNormal.z);
    glVertex3f(_nose.left.x,  _nose.left.y,  _nose.left.z  ); 
    glVertex3f(_nose.right.x, _nose.right.y, _nose.right.z ); 
    glVertex3f(_nose.front.x, _nose.front.y, _nose.front.z ); 

    glEnd();
}




void Head::renderMouth() {

    float s = sqrt(_averageLoudness);

    glm::quat orientation = getOrientation();
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    glm::vec3 up    = orientation * IDENTITY_UP;
    glm::vec3 front = orientation * IDENTITY_FRONT;

    glm::vec3 r = right * _scale * BODY_BALL_RADIUS_HEAD_BASE * (0.30f + s * 0.0014f );
    glm::vec3 u = up * _scale * BODY_BALL_RADIUS_HEAD_BASE * (0.05f + s * 0.0040f );
    glm::vec3 f = front * _scale * BODY_BALL_RADIUS_HEAD_BASE *  0.09f;

    glm::vec3 middle      = _mouthPosition;
    glm::vec3 leftCorner  = _mouthPosition - r * 1.0f;
    glm::vec3 rightCorner = _mouthPosition + r * 1.0f;
    glm::vec3 leftTop     = _mouthPosition - r * 0.4f + u * 0.7f + f;
    glm::vec3 rightTop    = _mouthPosition + r * 0.4f + u * 0.7f + f;
    glm::vec3 leftBottom  = _mouthPosition - r * 0.4f - u * 1.0f + f * 0.7f;
    glm::vec3 rightBottom = _mouthPosition + r * 0.4f - u * 1.0f + f * 0.7f;
    
    // constrain all mouth vertices to a sphere slightly larger than the head...
    const float MOUTH_OFFSET_OFF_FACE = 0.003f;
    
    float constrainedRadius = _scale * BODY_BALL_RADIUS_HEAD_BASE + MOUTH_OFFSET_OFF_FACE;
    middle      = _position + glm::normalize(middle      - _position) * constrainedRadius;
    leftCorner  = _position + glm::normalize(leftCorner  - _position) * constrainedRadius;
    rightCorner = _position + glm::normalize(rightCorner - _position) * constrainedRadius;
    leftTop     = _position + glm::normalize(leftTop     - _position) * constrainedRadius;
    rightTop    = _position + glm::normalize(rightTop    - _position) * constrainedRadius;
    leftBottom  = _position + glm::normalize(leftBottom  - _position) * constrainedRadius;
    rightBottom = _position + glm::normalize(rightBottom - _position) * constrainedRadius;

    glColor3f(0.2f, 0.0f, 0.0f);
    
    glBegin(GL_TRIANGLES);             
    glVertex3f(leftCorner.x,  leftCorner.y,  leftCorner.z ); 
    glVertex3f(leftBottom.x,  leftBottom.y,  leftBottom.z ); 
    glVertex3f(leftTop.x,     leftTop.y,     leftTop.z    ); 
    glVertex3f(leftTop.x,     leftTop.y,     leftTop.z    ); 
    glVertex3f(middle.x,      middle.y,      middle.z     ); 
    glVertex3f(rightTop.x,    rightTop.y,    rightTop.z   ); 
    glVertex3f(leftTop.x,     leftTop.y,     leftTop.z    ); 
    glVertex3f(middle.x,      middle.y,      middle.z     ); 
    glVertex3f(leftBottom.x,  leftBottom.y,  leftBottom.z ); 
    glVertex3f(leftBottom.x,  leftBottom.y,  leftBottom.z ); 
    glVertex3f(middle.x,      middle.y,      middle.z     ); 
    glVertex3f(rightBottom.x, rightBottom.y, rightBottom.z); 
    glVertex3f(rightTop.x,    rightTop.y,    rightTop.z   ); 
    glVertex3f(middle.x,      middle.y,      middle.z     ); 
    glVertex3f(rightBottom.x, rightBottom.y, rightBottom.z); 
    glVertex3f(rightTop.x,    rightTop.y,    rightTop.z   ); 
    glVertex3f(rightBottom.x, rightBottom.y, rightBottom.z); 
    glVertex3f(rightCorner.x, rightCorner.y, rightCorner.z); 
    glEnd();
}

void Head::renderEyeBrows() {   

    float height = _scale * BODY_BALL_RADIUS_HEAD_BASE * 0.3f + _browAudioLift;
    float length = _scale * BODY_BALL_RADIUS_HEAD_BASE * 0.2f;
    float width  = _scale * BODY_BALL_RADIUS_HEAD_BASE * 0.07f;

    glColor3f(0.3f, 0.25f, 0.2f);

    glm::vec3 leftCorner  = _leftEyePosition;
    glm::vec3 rightCorner = _leftEyePosition;
    glm::vec3 leftTop     = _leftEyePosition;
    glm::vec3 rightTop    = _leftEyePosition;
    glm::vec3 leftBottom  = _leftEyePosition;
    glm::vec3 rightBottom = _leftEyePosition;
   
    glm::quat orientation = getOrientation();
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    glm::vec3 up    = orientation * IDENTITY_UP;
    glm::vec3 front = orientation * IDENTITY_FRONT;
    
    glm::vec3 r = right * length; 
    glm::vec3 u = up * height; 
    glm::vec3 t = up * (height + width); 
    glm::vec3 f = front * _scale * BODY_BALL_RADIUS_HEAD_BASE * -0.1f;
     
    for (int i = 0; i < 2; i++) {
    
        if ( i == 1 ) {
            leftCorner = rightCorner = leftTop = rightTop = leftBottom = rightBottom = _rightEyePosition;
        }
       
        leftCorner  -= r * 1.0f;
        rightCorner += r * 1.0f;
        leftTop     -= r * 0.4f;
        rightTop    += r * 0.4f;
        leftBottom  -= r * 0.4f;
        rightBottom += r * 0.4f;

        leftCorner  += u + f;
        rightCorner += u + f;
        leftTop     += t + f;
        rightTop    += t + f;
        leftBottom  += u + f;
        rightBottom += u + f;        
        
        glBegin(GL_TRIANGLES);             

        glVertex3f(leftCorner.x,  leftCorner.y,  leftCorner.z ); 
        glVertex3f(leftBottom.x,  leftBottom.y,  leftBottom.z ); 
        glVertex3f(leftTop.x,     leftTop.y,     leftTop.z    ); 
        glVertex3f(leftTop.x,     leftTop.y,     leftTop.z    ); 
        glVertex3f(rightTop.x,    rightTop.y,    rightTop.z   ); 
        glVertex3f(leftBottom.x,  leftBottom.y,  leftBottom.z ); 
        glVertex3f(rightTop.x,    rightTop.y,    rightTop.z   ); 
        glVertex3f(leftBottom.x,  leftBottom.y,  leftBottom.z ); 
        glVertex3f(rightBottom.x, rightBottom.y, rightBottom.z); 
        glVertex3f(rightTop.x,    rightTop.y,    rightTop.z   ); 
        glVertex3f(rightBottom.x, rightBottom.y, rightBottom.z); 
        glVertex3f(rightCorner.x, rightCorner.y, rightCorner.z); 

        glEnd();
    }
  }
  

void Head::renderEyeBalls() {                                 
    
    // render white ball of left eyeball
    glPushMatrix();
        glColor3fv(EYEBALL_COLOR);
        glTranslatef(_leftEyePosition.x, _leftEyePosition.y, _leftEyePosition.z);        
        glutSolidSphere(_scale * EYEBALL_RADIUS, 30, 30);
    glPopMatrix();
    
    //render white ball of right eyeball
    glPushMatrix();
        glColor3fv(EYEBALL_COLOR);
        glTranslatef(_rightEyePosition.x, _rightEyePosition.y, _rightEyePosition.z);
        glutSolidSphere(_scale * EYEBALL_RADIUS, 30, 30);
    glPopMatrix();

    _irisProgram.bind();
    
    _dilatedIrisTexture = _irisTexture->getDilatedTexture(_pupilDilation);
    glBindTexture(GL_TEXTURE_2D, _dilatedIrisTexture->getID());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    
    glEnable(GL_TEXTURE_2D);
    
    // render left iris
    glm::quat leftIrisRotation;
    glPushMatrix(); {
        glTranslatef(_leftEyePosition.x, _leftEyePosition.y, _leftEyePosition.z); //translate to eyeball position
        
        //rotate the eyeball to aim towards the lookat position
        leftIrisRotation = getEyeRotation(_leftEyePosition);
        glm::vec3 rotationAxis = glm::axis(leftIrisRotation);           
        glRotatef(glm::angle(leftIrisRotation), rotationAxis.x, rotationAxis.y, rotationAxis.z);
        glTranslatef(0.0f, 0.0f, -_scale * IRIS_PROTRUSION);
        glScalef(_scale * IRIS_RADIUS * 2.0f,
                 _scale * IRIS_RADIUS * 2.0f,
                 _scale * IRIS_RADIUS); // flatten the iris
        
        // this ugliness is simply to invert the model transform and get the eye position in model space
        _irisProgram.setUniform(_eyePositionLocation, (glm::inverse(leftIrisRotation) *
            (Application::getInstance()->getCamera()->getPosition() - _leftEyePosition) +
                glm::vec3(0.0f, 0.0f, _scale * IRIS_PROTRUSION)) * glm::vec3(1.0f / (_scale * IRIS_RADIUS * 2.0f),
                    1.0f / (_scale * IRIS_RADIUS * 2.0f), 1.0f / (_scale * IRIS_RADIUS)));
        
        glutSolidSphere(0.5f, 15, 15);
    }
    glPopMatrix();

    // render right iris
    glm::quat rightIrisRotation;
    glPushMatrix(); {
        glTranslatef(_rightEyePosition.x, _rightEyePosition.y, _rightEyePosition.z);  //translate to eyeball position       
        
        //rotate the eyeball to aim towards the lookat position
        rightIrisRotation = getEyeRotation(_rightEyePosition);
        glm::vec3 rotationAxis = glm::axis(rightIrisRotation);        
        glRotatef(glm::angle(rightIrisRotation), rotationAxis.x, rotationAxis.y, rotationAxis.z);
        glTranslatef(0.0f, 0.0f, -_scale * IRIS_PROTRUSION);
        glScalef(_scale * IRIS_RADIUS * 2.0f,
                 _scale * IRIS_RADIUS * 2.0f,
                 _scale * IRIS_RADIUS); // flatten the iris
        
        // this ugliness is simply to invert the model transform and get the eye position in model space
        _irisProgram.setUniform(_eyePositionLocation, (glm::inverse(rightIrisRotation) *
            (Application::getInstance()->getCamera()->getPosition() - _rightEyePosition) +
                glm::vec3(0.0f, 0.0f, _scale * IRIS_PROTRUSION)) * glm::vec3(1.0f / (_scale * IRIS_RADIUS * 2.0f),
                    1.0f / (_scale * IRIS_RADIUS * 2.0f), 1.0f / (_scale * IRIS_RADIUS)));
        
        glutSolidSphere(0.5f, 15, 15);
    }
    glPopMatrix();
    
    _irisProgram.release();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    
    glEnable(GL_RESCALE_NORMAL);
    glColor4f(_skinColor.x, _skinColor.y, _skinColor.z, _renderAlpha);
    
    // left eyelid
    glPushMatrix(); {
        glTranslatef(_leftEyePosition.x, _leftEyePosition.y, _leftEyePosition.z);  //translate to eyeball position
        glm::vec3 rotationAxis = glm::axis(leftIrisRotation);
        glRotatef(glm::angle(leftIrisRotation), rotationAxis.x, rotationAxis.y, rotationAxis.z);
        glScalef(_scale * EYELID_RADIUS, _scale * EYELID_RADIUS, _scale * EYELID_RADIUS);
        float angle = -67.5f - 50.0f * _leftEyeBlink;
        glRotatef(angle, 1, 0, 0);
        Application::getInstance()->getGeometryCache()->renderHemisphere(15, 10);
        glRotatef(glm::mix(-angle, 180.0f, max(0.0f, _leftEyeBlink)), 1, 0, 0);
        Application::getInstance()->getGeometryCache()->renderHemisphere(15, 10);
    }
    glPopMatrix();
    
    // right eyelid
    glPushMatrix(); {
        glTranslatef(_rightEyePosition.x, _rightEyePosition.y, _rightEyePosition.z);  //translate to eyeball position
        glm::vec3 rotationAxis = glm::axis(rightIrisRotation);
        glRotatef(glm::angle(rightIrisRotation), rotationAxis.x, rotationAxis.y, rotationAxis.z);
        glScalef(_scale * EYELID_RADIUS, _scale * EYELID_RADIUS, _scale * EYELID_RADIUS);
        float angle = -67.5f - 50.0f * _rightEyeBlink; 
        glRotatef(angle, 1, 0, 0);
        Application::getInstance()->getGeometryCache()->renderHemisphere(15, 10);
        glRotatef(glm::mix(-angle, 180.0f, max(0.0f, _rightEyeBlink)), 1, 0, 0);
        Application::getInstance()->getGeometryCache()->renderHemisphere(15, 10);
    }
    glPopMatrix();
    
    glDisable(GL_RESCALE_NORMAL);
}

void Head::renderLookatVectors(glm::vec3 leftEyePosition, glm::vec3 rightEyePosition, glm::vec3 lookatPosition) {

    Application::getInstance()->getGlowEffect()->begin();
    
    glLineWidth(2.0);
    glBegin(GL_LINES);
    glColor4f(0.2f, 0.2f, 0.2f, 1.f);
    glVertex3f(leftEyePosition.x, leftEyePosition.y, leftEyePosition.z);
    glColor4f(1.0f, 1.0f, 1.0f, 0.f);
    glVertex3f(lookatPosition.x, lookatPosition.y, lookatPosition.z);
    glColor4f(0.2f, 0.2f, 0.2f, 1.f);
    glVertex3f(rightEyePosition.x, rightEyePosition.y, rightEyePosition.z);
    glColor4f(1.0f, 1.0f, 1.0f, 0.f);
    glVertex3f(lookatPosition.x, lookatPosition.y, lookatPosition.z);
    glEnd();
    
    Application::getInstance()->getGlowEffect()->end();
}

void Head::updateHairPhysics(float deltaTime) {

    glm::quat orientation = getOrientation();
    glm::vec3 up    = orientation * IDENTITY_UP;
    glm::vec3 front = orientation * IDENTITY_FRONT;
    for (int t = 0; t < NUM_HAIR_TUFTS; t ++) {
        float fraction = (float)t / (float)(NUM_HAIR_TUFTS - 1);
        float angle = -20.0f + 40.0f * fraction;
        float radian = angle * PI_OVER_180;
        glm::vec3 baseDirection = front * sinf(radian) + up * cosf(radian);
        _hairTuft[t].setBasePosition (_position + _scale * BODY_BALL_RADIUS_HEAD_BASE * 0.9f * baseDirection);
        _hairTuft[t].setBaseDirection(baseDirection);
        _hairTuft[t].update(deltaTime);     
    }
}
