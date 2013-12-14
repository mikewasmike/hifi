//
//  Model.cpp
//  interface
//
//  Created by Andrzej Kapolka on 10/18/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <glm/gtx/transform.hpp>

#include <GeometryUtil.h>

#include "Application.h"
#include "Model.h"

using namespace std;

Model::Model(QObject* parent) :
    QObject(parent),
    _pupilDilation(0.0f)
{
    // we may have been created in the network thread, but we live in the main thread
    moveToThread(Application::getInstance()->thread());
}

Model::~Model() {
    deleteGeometry();
}

ProgramObject Model::_program;
ProgramObject Model::_normalMapProgram;
ProgramObject Model::_skinProgram;
ProgramObject Model::_skinNormalMapProgram;
int Model::_normalMapTangentLocation;
Model::SkinLocations Model::_skinLocations;
Model::SkinLocations Model::_skinNormalMapLocations;

void Model::initSkinProgram(ProgramObject& program, Model::SkinLocations& locations) {
    program.bind();
    locations.clusterMatrices = program.uniformLocation("clusterMatrices");
    locations.clusterIndices = program.attributeLocation("clusterIndices");
    locations.clusterWeights = program.attributeLocation("clusterWeights");
    locations.tangent = program.attributeLocation("tangent");
    program.setUniformValue("diffuseMap", 0);
    program.setUniformValue("normalMap", 1);
    program.release();
}

void Model::init() {
    if (!_program.isLinked()) {
        switchToResourcesParentIfRequired();
        _program.addShaderFromSourceFile(QGLShader::Vertex, "resources/shaders/model.vert");
        _program.addShaderFromSourceFile(QGLShader::Fragment, "resources/shaders/model.frag");
        _program.link();
        
        _program.bind();
        _program.setUniformValue("texture", 0);
        _program.release();
        
        _normalMapProgram.addShaderFromSourceFile(QGLShader::Vertex, "resources/shaders/model_normal_map.vert");
        _normalMapProgram.addShaderFromSourceFile(QGLShader::Fragment, "resources/shaders/model_normal_map.frag");
        _normalMapProgram.link();
        
        _normalMapProgram.bind();
        _normalMapProgram.setUniformValue("diffuseMap", 0);
        _normalMapProgram.setUniformValue("normalMap", 1);
        _normalMapTangentLocation = _normalMapProgram.attributeLocation("tangent");
        _normalMapProgram.release();
        
        _skinProgram.addShaderFromSourceFile(QGLShader::Vertex, "resources/shaders/skin_model.vert");
        _skinProgram.addShaderFromSourceFile(QGLShader::Fragment, "resources/shaders/model.frag");
        _skinProgram.link();
        
        initSkinProgram(_skinProgram, _skinLocations);
        
        _skinNormalMapProgram.addShaderFromSourceFile(QGLShader::Vertex, "resources/shaders/skin_model_normal_map.vert");
        _skinNormalMapProgram.addShaderFromSourceFile(QGLShader::Fragment, "resources/shaders/model_normal_map.frag");
        _skinNormalMapProgram.link();
        
        initSkinProgram(_skinNormalMapProgram, _skinNormalMapLocations);
    }
}

void Model::reset() {
    _resetStates = true;
    
    foreach (Model* attachment, _attachments) {
        attachment->reset();
    }
}

void Model::simulate(float deltaTime) {
    if (!isActive()) {
        return;
    }
    
    // set up world vertices on first simulate after load
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    if (_jointStates.isEmpty()) {
        foreach (const FBXJoint& joint, geometry.joints) {
            JointState state;
            state.rotation = joint.rotation;
            _jointStates.append(state);
        }
        foreach (const FBXMesh& mesh, geometry.meshes) {
            MeshState state;
            state.clusterMatrices.resize(mesh.clusters.size());
            if (mesh.springiness > 0.0f) {
                state.worldSpaceVertices.resize(mesh.vertices.size());
                state.vertexVelocities.resize(mesh.vertices.size());
                state.worldSpaceNormals.resize(mesh.vertices.size());
            }
            _meshStates.append(state);    
        }
        foreach (const FBXAttachment& attachment, geometry.attachments) {
            Model* model = new Model(this);
            model->init();
            model->setURL(attachment.url);
            _attachments.append(model);
        }
        _resetStates = true;
    }
    
    // update the world space transforms for all joints
    for (int i = 0; i < _jointStates.size(); i++) {
        updateJointState(i);
    }
    
    // update the attachment transforms and simulate them
    for (int i = 0; i < _attachments.size(); i++) {
        const FBXAttachment& attachment = geometry.attachments.at(i);
        Model* model = _attachments.at(i);
        
        glm::vec3 jointTranslation = _translation;
        glm::quat jointRotation = _rotation;
        getJointPosition(attachment.jointIndex, jointTranslation);
        getJointRotation(attachment.jointIndex, jointRotation);
        
        model->setTranslation(jointTranslation + jointRotation * attachment.translation * _scale);
        model->setRotation(jointRotation * attachment.rotation);
        model->setScale(_scale * attachment.scale);
        
        model->simulate(deltaTime);
    }
    
    for (int i = 0; i < _meshStates.size(); i++) {
        MeshState& state = _meshStates[i];
        const FBXMesh& mesh = geometry.meshes.at(i);
        for (int j = 0; j < mesh.clusters.size(); j++) {
            const FBXCluster& cluster = mesh.clusters.at(j);
            state.clusterMatrices[j] = _jointStates[cluster.jointIndex].transform * cluster.inverseBindMatrix;
        }
        int vertexCount = state.worldSpaceVertices.size();
        if (vertexCount == 0) {
            continue;
        }
        glm::vec3* destVertices = state.worldSpaceVertices.data();
        glm::vec3* destVelocities = state.vertexVelocities.data();
        glm::vec3* destNormals = state.worldSpaceNormals.data();
        
        const glm::vec3* sourceVertices = mesh.vertices.constData();
        if (!mesh.blendshapes.isEmpty()) {
            _blendedVertices.resize(max(_blendedVertices.size(), vertexCount));
            memcpy(_blendedVertices.data(), mesh.vertices.constData(), vertexCount * sizeof(glm::vec3));
            
            // blend in each coefficient
            for (int j = 0; j < _blendshapeCoefficients.size(); j++) {
                float coefficient = _blendshapeCoefficients[j];
                if (coefficient == 0.0f || j >= mesh.blendshapes.size() || mesh.blendshapes[j].vertices.isEmpty()) {
                    continue;
                }
                const glm::vec3* vertex = mesh.blendshapes[j].vertices.constData();
                for (const int* index = mesh.blendshapes[j].indices.constData(),
                        *end = index + mesh.blendshapes[j].indices.size(); index != end; index++, vertex++) {
                    _blendedVertices[*index] += *vertex * coefficient;
                }
            }
            sourceVertices = _blendedVertices.constData();
        }
        glm::mat4 transform = glm::translate(_translation);
        if (mesh.clusters.size() > 1) {
            _blendedVertices.resize(max(_blendedVertices.size(), vertexCount));

            // skin each vertex
            const glm::vec4* clusterIndices = mesh.clusterIndices.constData();
            const glm::vec4* clusterWeights = mesh.clusterWeights.constData();
            for (int j = 0; j < vertexCount; j++) {
                _blendedVertices[j] =
                    glm::vec3(state.clusterMatrices[clusterIndices[j][0]] *
                        glm::vec4(sourceVertices[j], 1.0f)) * clusterWeights[j][0] +
                    glm::vec3(state.clusterMatrices[clusterIndices[j][1]] *
                        glm::vec4(sourceVertices[j], 1.0f)) * clusterWeights[j][1] +
                    glm::vec3(state.clusterMatrices[clusterIndices[j][2]] *
                        glm::vec4(sourceVertices[j], 1.0f)) * clusterWeights[j][2] +
                    glm::vec3(state.clusterMatrices[clusterIndices[j][3]] *
                        glm::vec4(sourceVertices[j], 1.0f)) * clusterWeights[j][3];
            }
            sourceVertices = _blendedVertices.constData();
            
        } else {
            transform = state.clusterMatrices[0];
        }
        if (_resetStates) {
            for (int j = 0; j < vertexCount; j++) {
                destVertices[j] = glm::vec3(transform * glm::vec4(sourceVertices[j], 1.0f));        
                destVelocities[j] = glm::vec3();
            }        
        } else {
            const float SPRINGINESS_MULTIPLIER = 200.0f;
            const float DAMPING = 5.0f;
            for (int j = 0; j < vertexCount; j++) {
                destVelocities[j] += ((glm::vec3(transform * glm::vec4(sourceVertices[j], 1.0f)) - destVertices[j]) *
                    mesh.springiness * SPRINGINESS_MULTIPLIER - destVelocities[j] * DAMPING) * deltaTime;
                destVertices[j] += destVelocities[j] * deltaTime;
            }
        }
        for (int j = 0; j < vertexCount; j++) {
            destNormals[j] = glm::vec3();
            
            const glm::vec3& middle = destVertices[j];
            for (QVarLengthArray<QPair<int, int>, 4>::const_iterator connection = mesh.vertexConnections.at(j).constBegin(); 
                    connection != mesh.vertexConnections.at(j).constEnd(); connection++) {
                destNormals[j] += glm::normalize(glm::cross(destVertices[connection->second] - middle,
                    destVertices[connection->first] - middle));
            }
        }
    }
    _resetStates = false;
}

bool Model::render(float alpha) {
    // render the attachments
    foreach (Model* attachment, _attachments) {
        attachment->render(alpha);
    }
    if (_meshStates.isEmpty()) {
        return false;
    }
    
    // set up blended buffer ids on first render after load/simulate
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    const QVector<NetworkMesh>& networkMeshes = _geometry->getMeshes();
    if (_blendedVertexBufferIDs.isEmpty()) {
        foreach (const FBXMesh& mesh, geometry.meshes) {
            GLuint id = 0;
            if (!mesh.blendshapes.isEmpty() || mesh.springiness > 0.0f) {
                glGenBuffers(1, &id);
                glBindBuffer(GL_ARRAY_BUFFER, id);
                glBufferData(GL_ARRAY_BUFFER, (mesh.vertices.size() + mesh.normals.size()) * sizeof(glm::vec3),
                    NULL, GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
            _blendedVertexBufferIDs.append(id);
            
            QVector<QSharedPointer<Texture> > dilated;
            dilated.resize(mesh.parts.size());
            _dilatedTextures.append(dilated);
        }
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    
    glDisable(GL_COLOR_MATERIAL);
    
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    
    for (int i = 0; i < networkMeshes.size(); i++) {
        const NetworkMesh& networkMesh = networkMeshes.at(i);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, networkMesh.indexBufferID);
        
        const FBXMesh& mesh = geometry.meshes.at(i);    
        int vertexCount = mesh.vertices.size();
        
        glBindBuffer(GL_ARRAY_BUFFER, networkMesh.vertexBufferID);
      
        ProgramObject* program = &_program;
        ProgramObject* skinProgram = &_skinProgram;
        SkinLocations* skinLocations = &_skinLocations;
        if (!mesh.tangents.isEmpty()) {
            program = &_normalMapProgram;
            skinProgram = &_skinNormalMapProgram;
            skinLocations = &_skinNormalMapLocations;
        }
        
        const MeshState& state = _meshStates.at(i);
        ProgramObject* activeProgram = program;
        int tangentLocation = _normalMapTangentLocation;
        if (state.worldSpaceVertices.isEmpty()) {
            glPushMatrix();
            Application::getInstance()->loadTranslatedViewMatrix(_translation);
            
            if (state.clusterMatrices.size() > 1) {
                skinProgram->bind();
                glUniformMatrix4fvARB(skinLocations->clusterMatrices, state.clusterMatrices.size(), false,
                    (const float*)state.clusterMatrices.constData());
                int offset = (mesh.tangents.size() + mesh.colors.size()) * sizeof(glm::vec3) +
                    mesh.texCoords.size() * sizeof(glm::vec2) +
                    (mesh.blendshapes.isEmpty() ? vertexCount * 2 * sizeof(glm::vec3) : 0);
                skinProgram->setAttributeBuffer(skinLocations->clusterIndices, GL_FLOAT, offset, 4);
                skinProgram->setAttributeBuffer(skinLocations->clusterWeights, GL_FLOAT,
                    offset + vertexCount * sizeof(glm::vec4), 4);
                skinProgram->enableAttributeArray(skinLocations->clusterIndices);
                skinProgram->enableAttributeArray(skinLocations->clusterWeights);
                activeProgram = skinProgram;
                tangentLocation = skinLocations->tangent;
         
            } else {    
                glMultMatrixf((const GLfloat*)&state.clusterMatrices[0]);
                program->bind();
            }
        } else {
            program->bind();
        }

        if (mesh.blendshapes.isEmpty() && mesh.springiness == 0.0f) {
            if (!mesh.tangents.isEmpty()) {
                activeProgram->setAttributeBuffer(tangentLocation, GL_FLOAT, vertexCount * 2 * sizeof(glm::vec3), 3);
                activeProgram->enableAttributeArray(tangentLocation);
            }
            glColorPointer(3, GL_FLOAT, 0, (void*)(vertexCount * 2 * sizeof(glm::vec3) +
                mesh.tangents.size() * sizeof(glm::vec3)));
            glTexCoordPointer(2, GL_FLOAT, 0, (void*)(vertexCount * 2 * sizeof(glm::vec3) +
                (mesh.tangents.size() + mesh.colors.size()) * sizeof(glm::vec3)));    
        
        } else {
            if (!mesh.tangents.isEmpty()) {
                activeProgram->setAttributeBuffer(tangentLocation, GL_FLOAT, 0, 3);
                activeProgram->enableAttributeArray(tangentLocation);
            }
            glColorPointer(3, GL_FLOAT, 0, (void*)(mesh.tangents.size() * sizeof(glm::vec3)));
            glTexCoordPointer(2, GL_FLOAT, 0, (void*)((mesh.tangents.size() + mesh.colors.size()) * sizeof(glm::vec3)));
            glBindBuffer(GL_ARRAY_BUFFER, _blendedVertexBufferIDs.at(i));
            
            if (!state.worldSpaceVertices.isEmpty()) {
                glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(glm::vec3), state.worldSpaceVertices.constData());
                glBufferSubData(GL_ARRAY_BUFFER, vertexCount * sizeof(glm::vec3),
                    vertexCount * sizeof(glm::vec3), state.worldSpaceNormals.constData());
                
            } else {
                _blendedVertices.resize(max(_blendedVertices.size(), vertexCount));
                _blendedNormals.resize(_blendedVertices.size());
                memcpy(_blendedVertices.data(), mesh.vertices.constData(), vertexCount * sizeof(glm::vec3));
                memcpy(_blendedNormals.data(), mesh.normals.constData(), vertexCount * sizeof(glm::vec3));
                
                // blend in each coefficient
                for (int j = 0; j < _blendshapeCoefficients.size(); j++) {
                    float coefficient = _blendshapeCoefficients[j];
                    if (coefficient == 0.0f || j >= mesh.blendshapes.size() || mesh.blendshapes[j].vertices.isEmpty()) {
                        continue;
                    }
                    const float NORMAL_COEFFICIENT_SCALE = 0.01f;
                    float normalCoefficient = coefficient * NORMAL_COEFFICIENT_SCALE;
                    const glm::vec3* vertex = mesh.blendshapes[j].vertices.constData();
                    const glm::vec3* normal = mesh.blendshapes[j].normals.constData();
                    for (const int* index = mesh.blendshapes[j].indices.constData(),
                            *end = index + mesh.blendshapes[j].indices.size(); index != end; index++, vertex++, normal++) {
                        _blendedVertices[*index] += *vertex * coefficient;
                        _blendedNormals[*index] += *normal * normalCoefficient;
                    }
                }
        
                glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(glm::vec3), _blendedVertices.constData());
                glBufferSubData(GL_ARRAY_BUFFER, vertexCount * sizeof(glm::vec3),
                    vertexCount * sizeof(glm::vec3), _blendedNormals.constData());
            }
        }
        glVertexPointer(3, GL_FLOAT, 0, 0);
        glNormalPointer(GL_FLOAT, 0, (void*)(vertexCount * sizeof(glm::vec3)));
        
        if (!mesh.colors.isEmpty()) {
            glEnableClientState(GL_COLOR_ARRAY);
        } else {
            glColor3f(1.0f, 1.0f, 1.0f);
        }
        if (!mesh.texCoords.isEmpty()) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        
        qint64 offset = 0;
        for (int j = 0; j < networkMesh.parts.size(); j++) {
            const NetworkMeshPart& networkPart = networkMesh.parts.at(j);
            const FBXMeshPart& part = mesh.parts.at(j);
            
            // apply material properties
            glm::vec4 diffuse = glm::vec4(part.diffuseColor, alpha);
            glm::vec4 specular = glm::vec4(part.specularColor, alpha);
            glMaterialfv(GL_FRONT, GL_AMBIENT, (const float*)&diffuse);
            glMaterialfv(GL_FRONT, GL_DIFFUSE, (const float*)&diffuse);
            glMaterialfv(GL_FRONT, GL_SPECULAR, (const float*)&specular);
            glMaterialf(GL_FRONT, GL_SHININESS, part.shininess);
        
            Texture* diffuseMap = networkPart.diffuseTexture.data();
            if (mesh.isEye) {
                if (diffuseMap != NULL) {
                    diffuseMap = (_dilatedTextures[i][j] =
                        static_cast<DilatableNetworkTexture*>(diffuseMap)->getDilatedTexture(_pupilDilation)).data();
                }
            }
            glBindTexture(GL_TEXTURE_2D, diffuseMap == NULL ?
                Application::getInstance()->getTextureCache()->getWhiteTextureID() : diffuseMap->getID());
            
            if (!mesh.tangents.isEmpty()) {
                glActiveTexture(GL_TEXTURE1);                
                Texture* normalMap = networkPart.normalTexture.data();
                glBindTexture(GL_TEXTURE_2D, normalMap == NULL ?
                    Application::getInstance()->getTextureCache()->getBlueTextureID() : normalMap->getID());
                glActiveTexture(GL_TEXTURE0);
            }
            
            glDrawRangeElementsEXT(GL_QUADS, 0, vertexCount - 1, part.quadIndices.size(), GL_UNSIGNED_INT, (void*)offset);
            offset += part.quadIndices.size() * sizeof(int);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, vertexCount - 1, part.triangleIndices.size(),
                GL_UNSIGNED_INT, (void*)offset);
            offset += part.triangleIndices.size() * sizeof(int);
        }
        
        if (!mesh.colors.isEmpty()) {
            glDisableClientState(GL_COLOR_ARRAY);
        }
        if (!mesh.texCoords.isEmpty()) {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        
        if (!mesh.tangents.isEmpty()) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
            
            activeProgram->disableAttributeArray(tangentLocation);
        }
        
        if (state.worldSpaceVertices.isEmpty()) {
            if (state.clusterMatrices.size() > 1) {
                skinProgram->disableAttributeArray(skinLocations->clusterIndices);
                skinProgram->disableAttributeArray(skinLocations->clusterWeights);  
            } 
            glPopMatrix();
        }
        activeProgram->release();
    }
    
    // deactivate vertex arrays after drawing
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    
    glDisable(GL_ALPHA_TEST);
    
    // bind with 0 to switch back to normal operation
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // restore all the default material settings
    Application::getInstance()->setupWorldLight();

    return true;
}

int Model::getParentJointIndex(int jointIndex) const {
    return (isActive() && jointIndex != -1) ? _geometry->getFBXGeometry().joints.at(jointIndex).parentIndex : -1;
}

int Model::getLastFreeJointIndex(int jointIndex) const {
    return (isActive() && jointIndex != -1) ? _geometry->getFBXGeometry().joints.at(jointIndex).freeLineage.last() : -1;
}

bool Model::getHeadPosition(glm::vec3& headPosition) const {
    return isActive() && getJointPosition(_geometry->getFBXGeometry().headJointIndex, headPosition);
}

bool Model::getNeckPosition(glm::vec3& neckPosition) const {
    return isActive() && getJointPosition(_geometry->getFBXGeometry().neckJointIndex, neckPosition);
}

bool Model::getNeckRotation(glm::quat& neckRotation) const {
    return isActive() && getJointRotation(_geometry->getFBXGeometry().neckJointIndex, neckRotation);
}

bool Model::getEyePositions(glm::vec3& firstEyePosition, glm::vec3& secondEyePosition) const {
    if (!isActive()) {
        return false;
    }
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    return getJointPosition(geometry.leftEyeJointIndex, firstEyePosition) &&
        getJointPosition(geometry.rightEyeJointIndex, secondEyePosition);
}

bool Model::setLeftHandPosition(const glm::vec3& position) {
    return setJointPosition(getLeftHandJointIndex(), position);
}

bool Model::restoreLeftHandPosition(float percent) {
    return restoreJointPosition(getLeftHandJointIndex(), percent);
}

bool Model::setLeftHandRotation(const glm::quat& rotation) {
    return setJointRotation(getLeftHandJointIndex(), rotation);
}

float Model::getLeftArmLength() const {
    return getLimbLength(getLeftHandJointIndex());
}

bool Model::setRightHandPosition(const glm::vec3& position) {
    return setJointPosition(getRightHandJointIndex(), position);
}

bool Model::restoreRightHandPosition(float percent) {
    return restoreJointPosition(getRightHandJointIndex(), percent);
}

bool Model::setRightHandRotation(const glm::quat& rotation) {
    return setJointRotation(getRightHandJointIndex(), rotation);
}

float Model::getRightArmLength() const {
    return getLimbLength(getRightHandJointIndex());
}

void Model::setURL(const QUrl& url) {
    // don't recreate the geometry if it's the same URL
    if (_url == url) {
        return;
    }
    _url = url;

    // delete our local geometry and custom textures
    deleteGeometry();
    _dilatedTextures.clear();
    
    _geometry = Application::getInstance()->getGeometryCache()->getGeometry(url);
}

glm::vec4 Model::computeAverageColor() const {
    return _geometry ? _geometry->computeAverageColor() : glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
}

bool Model::findSpherePenetration(const glm::vec3& penetratorCenter, float penetratorRadius,
        glm::vec3& penetration, float boneScale, int skipIndex) const {
    const glm::vec3 relativeCenter = penetratorCenter - _translation;
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    bool didPenetrate = false;
    glm::vec3 totalPenetration;
    float radiusScale = extractUniformScale(_scale) * boneScale;
    for (int i = 0; i < _jointStates.size(); i++) {
        const FBXJoint& joint = geometry.joints[i];
        glm::vec3 end = extractTranslation(_jointStates[i].transform);
        float endRadius = joint.boneRadius * radiusScale;
        glm::vec3 start = end;
        float startRadius = joint.boneRadius * radiusScale;
        glm::vec3 bonePenetration;
        if (joint.parentIndex != -1) {
            if (skipIndex != -1) {
                int ancestorIndex = joint.parentIndex;
                do {
                    if (ancestorIndex == skipIndex) {
                        goto outerContinue;
                    }
                    ancestorIndex = geometry.joints[ancestorIndex].parentIndex;
                    
                } while (ancestorIndex != -1);
            }
            start = extractTranslation(_jointStates[joint.parentIndex].transform);
            startRadius = geometry.joints[joint.parentIndex].boneRadius * radiusScale;
        }
        if (findSphereCapsuleConePenetration(relativeCenter, penetratorRadius, start, end,
                startRadius, endRadius, bonePenetration)) {
            totalPenetration = addPenetrations(totalPenetration, bonePenetration);
            didPenetrate = true; 
        }
        outerContinue: ;
    }
    if (didPenetrate) {
        penetration = totalPenetration;
        return true;
    }
    return false;
}

void Model::updateJointState(int index) {
    JointState& state = _jointStates[index];
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    const FBXJoint& joint = geometry.joints.at(index);
    
    if (joint.parentIndex == -1) {
        glm::mat4 baseTransform = glm::mat4_cast(_rotation) * glm::scale(_scale) * glm::translate(_offset);
    
        glm::quat combinedRotation = joint.preRotation * state.rotation * joint.postRotation;    
        state.transform = baseTransform * geometry.offset * joint.preTransform *
            glm::mat4_cast(combinedRotation) * joint.postTransform;
        state.combinedRotation = _rotation * combinedRotation;
    
    } else {
        const JointState& parentState = _jointStates.at(joint.parentIndex);
        if (index == geometry.leanJointIndex) {
            maybeUpdateLeanRotation(parentState, joint, state);
        
        } else if (index == geometry.neckJointIndex) {
            maybeUpdateNeckRotation(parentState, joint, state);    
                
        } else if (index == geometry.leftEyeJointIndex || index == geometry.rightEyeJointIndex) {
            maybeUpdateEyeRotation(parentState, joint, state);
        }
        glm::quat combinedRotation = joint.preRotation * state.rotation * joint.postRotation;    
        state.transform = parentState.transform * joint.preTransform *
            glm::mat4_cast(combinedRotation) * joint.postTransform;
        state.combinedRotation = parentState.combinedRotation * combinedRotation;
    }
}

void Model::maybeUpdateLeanRotation(const JointState& parentState, const FBXJoint& joint, JointState& state) {
    // nothing by default
}

void Model::maybeUpdateNeckRotation(const JointState& parentState, const FBXJoint& joint, JointState& state) {
    // nothing by default
}

void Model::maybeUpdateEyeRotation(const JointState& parentState, const FBXJoint& joint, JointState& state) {
    // nothing by default
}

bool Model::getJointPosition(int jointIndex, glm::vec3& position) const {
    if (jointIndex == -1 || _jointStates.isEmpty()) {
        return false;
    }
    position = _translation + extractTranslation(_jointStates[jointIndex].transform);
    return true;
}

bool Model::getJointRotation(int jointIndex, glm::quat& rotation, bool fromBind) const {
    if (jointIndex == -1 || _jointStates.isEmpty()) {
        return false;
    }
    rotation = _jointStates[jointIndex].combinedRotation *
        (fromBind ? _geometry->getFBXGeometry().joints[jointIndex].inverseBindRotation :
            _geometry->getFBXGeometry().joints[jointIndex].inverseDefaultRotation);
    return true;
}

bool Model::setJointPosition(int jointIndex, const glm::vec3& position, int lastFreeIndex,
        bool allIntermediatesFree, const glm::vec3& alignment) {
    if (jointIndex == -1 || _jointStates.isEmpty()) {
        return false;
    }
    glm::vec3 relativePosition = position - _translation;
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    const QVector<int>& freeLineage = geometry.joints.at(jointIndex).freeLineage;
    if (lastFreeIndex == -1) {
        lastFreeIndex = freeLineage.last();
    }

    // this is a cyclic coordinate descent algorithm: see
    // http://www.ryanjuckett.com/programming/animation/21-cyclic-coordinate-descent-in-2d
    const int ITERATION_COUNT = 1;
    glm::vec3 worldAlignment = _rotation * alignment;
    for (int i = 0; i < ITERATION_COUNT; i++) {
        // first, we go from the joint upwards, rotating the end as close as possible to the target
        glm::vec3 endPosition = extractTranslation(_jointStates[jointIndex].transform);
        for (int j = 1; freeLineage.at(j - 1) != lastFreeIndex; j++) {
            int index = freeLineage.at(j);
            const FBXJoint& joint = geometry.joints.at(index);
            if (!(joint.isFree || allIntermediatesFree)) {
                continue;
            }
            JointState& state = _jointStates[index];
            glm::vec3 jointPosition = extractTranslation(state.transform);
            glm::vec3 jointVector = endPosition - jointPosition;
            glm::quat oldCombinedRotation = state.combinedRotation;
            applyRotationDelta(index, rotationBetween(jointVector, relativePosition - jointPosition));
            endPosition = state.combinedRotation * glm::inverse(oldCombinedRotation) * jointVector + jointPosition;
            if (alignment != glm::vec3() && j > 1) {
                jointVector = endPosition - jointPosition;
                glm::vec3 positionSum;
                for (int k = j - 1; k > 0; k--) {
                    int index = freeLineage.at(k);
                    updateJointState(index);
                    positionSum += extractTranslation(_jointStates.at(index).transform);
                }
                glm::vec3 projectedCenterOfMass = glm::cross(jointVector,
                    glm::cross(positionSum / (j - 1.0f) - jointPosition, jointVector));
                glm::vec3 projectedAlignment = glm::cross(jointVector, glm::cross(worldAlignment, jointVector));
                const float LENGTH_EPSILON = 0.001f;
                if (glm::length(projectedCenterOfMass) > LENGTH_EPSILON && glm::length(projectedAlignment) > LENGTH_EPSILON) {
                    applyRotationDelta(index, rotationBetween(projectedCenterOfMass, projectedAlignment));
                }
            }
        }       
    }
     
    // now update the joint states from the top
    for (int j = freeLineage.size() - 1; j >= 0; j--) {
        updateJointState(freeLineage.at(j));
    }
        
    return true;
}

bool Model::setJointRotation(int jointIndex, const glm::quat& rotation, bool fromBind) {
    if (jointIndex == -1 || _jointStates.isEmpty()) {
        return false;
    }
    JointState& state = _jointStates[jointIndex];
    state.rotation = state.rotation * glm::inverse(state.combinedRotation) * rotation *
        glm::inverse(fromBind ? _geometry->getFBXGeometry().joints.at(jointIndex).inverseBindRotation :
            _geometry->getFBXGeometry().joints.at(jointIndex).inverseDefaultRotation);
    return true;
}

bool Model::restoreJointPosition(int jointIndex, float percent) {
    if (jointIndex == -1 || _jointStates.isEmpty()) {
        return false;
    }
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    const QVector<int>& freeLineage = geometry.joints.at(jointIndex).freeLineage;
    
    foreach (int index, freeLineage) {
        _jointStates[index].rotation = safeMix(_jointStates[index].rotation, geometry.joints.at(index).rotation, percent);
    }
    return true;
}

float Model::getLimbLength(int jointIndex) const {
    if (jointIndex == -1 || _jointStates.isEmpty()) {
        return 0.0f;
    }
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    const QVector<int>& freeLineage = geometry.joints.at(jointIndex).freeLineage;
    float length = 0.0f;
    float lengthScale = (_scale.x + _scale.y + _scale.z) / 3.0f;
    for (int i = freeLineage.size() - 2; i >= 0; i--) {
        length += geometry.joints.at(freeLineage.at(i)).distanceToParent * lengthScale;
    }
    return length;
}

void Model::applyRotationDelta(int jointIndex, const glm::quat& delta, bool constrain) {
    JointState& state = _jointStates[jointIndex];
    const FBXJoint& joint = _geometry->getFBXGeometry().joints[jointIndex];
    if (!constrain || (joint.rotationMin == glm::vec3(-180.0f, -180.0f, -180.0f) &&
            joint.rotationMax == glm::vec3(180.0f, 180.0f, 180.0f))) {
        // no constraints
        state.rotation = state.rotation * glm::inverse(state.combinedRotation) * delta * state.combinedRotation;
        state.combinedRotation = delta * state.combinedRotation;
        return;
    }
    glm::quat newRotation = glm::quat(glm::radians(glm::clamp(safeEulerAngles(state.rotation *
        glm::inverse(state.combinedRotation) * delta * state.combinedRotation), joint.rotationMin, joint.rotationMax)));
    state.combinedRotation = state.combinedRotation * glm::inverse(state.rotation) * newRotation;
    state.rotation = newRotation;
}

void Model::renderCollisionProxies(float alpha) {
    glPushMatrix();
    Application::getInstance()->loadTranslatedViewMatrix(_translation);

    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    float uniformScale = extractUniformScale(_scale);
    for (int i = 0; i < _jointStates.size(); i++) {
        glPushMatrix();
        
        glm::vec3 position = extractTranslation(_jointStates[i].transform);
        glTranslatef(position.x, position.y, position.z);
        
        glm::quat rotation;
        getJointRotation(i, rotation);
        glm::vec3 axis = glm::axis(rotation);
        glRotatef(glm::angle(rotation), axis.x, axis.y, axis.z);
        
        glColor4f(0.75f, 0.75f, 0.75f, alpha);
        float scaledRadius = geometry.joints[i].boneRadius * uniformScale;
        const int BALL_SUBDIVISIONS = 10;
        glutSolidSphere(scaledRadius, BALL_SUBDIVISIONS, BALL_SUBDIVISIONS);
        
        glPopMatrix();
        
        int parentIndex = geometry.joints[i].parentIndex;
        if (parentIndex != -1) {
            Avatar::renderJointConnectingCone(extractTranslation(_jointStates[parentIndex].transform), position,
                geometry.joints[parentIndex].boneRadius * uniformScale, scaledRadius);
        }
    }
    
    glPopMatrix();
}

void Model::setJointTranslation(int jointIndex, int parentIndex, int childIndex, const glm::vec3& translation) {
    const FBXGeometry& geometry = _geometry->getFBXGeometry();
    JointState& state = _jointStates[jointIndex];
    if (childIndex != -1 && geometry.joints.at(jointIndex).isFree) {
        // if there's a child, then I must adjust *my* rotation
        glm::vec3 childTranslation = extractTranslation(_jointStates.at(childIndex).transform);
        applyRotationDelta(jointIndex, rotationBetween(childTranslation - extractTranslation(state.transform),
            childTranslation - translation));
    }
    if (parentIndex != -1 && geometry.joints.at(parentIndex).isFree) {
        // if there's a parent, then I must adjust *its* rotation
        JointState& parent = _jointStates[parentIndex];
        glm::vec3 parentTranslation = extractTranslation(parent.transform);
        applyRotationDelta(parentIndex, rotationBetween(extractTranslation(state.transform) - parentTranslation, 
            translation - parentTranslation));
    }
    ::setTranslation(state.transform, translation);
}

void Model::deleteGeometry() {
    foreach (Model* attachment, _attachments) {
        delete attachment;
    }
    _attachments.clear();
    foreach (GLuint id, _blendedVertexBufferIDs) {
        glDeleteBuffers(1, &id);
    }
    _blendedVertexBufferIDs.clear();
    _jointStates.clear();
    _meshStates.clear();
}
