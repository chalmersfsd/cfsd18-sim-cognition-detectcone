/**
* Copyright (C) 2017 Chalmers Revere
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
* USA.
*/

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

#include "detectcone.hpp"

DetectCone::DetectCone(std::map<std::string, std::string> commandlineArguments, cluon::OD4Session &od4) :
m_od4(od4)
, m_senderStamp{(commandlineArguments["id"].size() != 0) ? (static_cast<int>(std::stoi(commandlineArguments["id"]))) : (231)}
, m_detectRange{(commandlineArguments["detectRange"].size() != 0) ? (static_cast<float>(std::stof(commandlineArguments["detectRange"]))) : (11.5f)}
, m_detectWidth{(commandlineArguments["detectWidth"].size() != 0) ? (static_cast<float>(std::stof(commandlineArguments["detectWidth"]))) : (4.0f)}
, m_fakeSlamActivated{(commandlineArguments["fakeSlamActivated"].size() != 0) ? (std::stoi(commandlineArguments["fakeSlamActivated"])==1) : (true)}
, m_nConesFakeSlam{(commandlineArguments["nConesFakeSlam"].size() != 0) ? (static_cast<int>(std::stoi(commandlineArguments["nConesFakeSlam"]))) : (5)}
, m_startX{(commandlineArguments["startX"].size() != 0) ? (static_cast<float>(std::stof(commandlineArguments["startX"]))) : (0.0f)}
, m_startY{(commandlineArguments["startY"].size() != 0) ? (static_cast<float>(std::stof(commandlineArguments["startY"]))) : (0.0f)}
, m_startHeading{(commandlineArguments["startHeading"].size() != 0) ? (static_cast<int>(std::stof(commandlineArguments["startHeading"]))) : (0.0f)}
, m_filename{(commandlineArguments["mapFilename"].size() != 0) ? (commandlineArguments["mapFilename"]) : ("trackFSG.txt")}
, m_heading()
, m_location()
, m_leftCones()
, m_rightCones()
, m_smallCones()
, m_bigCones()
, m_orangeVisibleInSlam()
, m_locationMutex()
, m_sendId()
{
  m_sendId = rand();
  setUp();
  std::cout<<"DetectCone set up with "<<commandlineArguments.size()<<" commandlineArguments: "<<std::endl;
  for (std::map<std::string, std::string >::iterator it = commandlineArguments.begin();it !=commandlineArguments.end();it++){
    std::cout<<it->first<<" "<<it->second<<std::endl;
  }
}

DetectCone::~DetectCone()
{
}

void DetectCone::nextContainer(cluon::data::Envelope &a_container)
{
  if (a_container.dataType() == opendlv::sim::Frame::ID())
  {
    auto frame = cluon::extractMessage<opendlv::sim::Frame>(std::move(a_container));
    float x = frame.x();
    float y = frame.y();
    float yaw = frame.yaw();
    //std::cout<<"DetectCone received: " <<"x: "<<x<<" y: "<<y<<" yaw: "<<yaw<<std::endl;
    {
      std::unique_lock<std::mutex> lockLocation(m_locationMutex);
      m_location << x,y;
      m_heading = yaw;
    }
  }

}

void DetectCone::body()
{  //std::cout<<"DetectCone body: " <<"x: "<<m_location(0)<<" y: "<<m_location(1)<<" yaw: "<<m_heading<<std::endl;
    Eigen::ArrayXXf locationCopy;
    float headingCopy;
    {
      std::unique_lock<std::mutex> lockLocation(m_locationMutex);
      locationCopy = m_location;
      headingCopy = m_heading;
    }
    Eigen::ArrayXXf detectedConesLeft, detectedConesRight, detectedConesSmall, detectedConesBig;

    if(m_fakeSlamActivated)
    {
      detectedConesLeft = DetectCone::simConeDetectorSlam(m_leftCones, locationCopy, headingCopy, m_nConesFakeSlam);
      detectedConesRight = DetectCone::simConeDetectorSlam(m_rightCones, locationCopy, headingCopy, m_nConesFakeSlam);

      if(m_orangeVisibleInSlam)
      {
        // If the orange cones are set to visible in the detection they will be transformed into local coordinates and stored
        m_orangeVisibleInSlam = false;
        Eigen::MatrixXf rotationMatrix(2,2);
        rotationMatrix << std::cos(-headingCopy),-std::sin(-headingCopy),
                          std::sin(-headingCopy),std::cos(-headingCopy);

        Eigen::ArrayXXf tmpLocationSmall(m_smallCones.rows(),2);
        (tmpLocationSmall.col(0)).fill(locationCopy(0));
        (tmpLocationSmall.col(1)).fill(locationCopy(1));
        Eigen::ArrayXXf tmpLocationBig(m_bigCones.rows(),2);
        (tmpLocationBig.col(0)).fill(locationCopy(0));
        (tmpLocationBig.col(1)).fill(locationCopy(1));

        detectedConesSmall = ((rotationMatrix*(((m_smallCones-tmpLocationSmall).matrix()).transpose())).transpose()).array();
        detectedConesBig = ((rotationMatrix*(((m_bigCones-tmpLocationBig).matrix()).transpose())).transpose()).array();
      }
      else
      {
        // Otherwise no orange cones are stored
        detectedConesSmall.resize(0,2);
        detectedConesBig.resize(0,2);
      } // End of else
    }
    else
    {
      // If slam detection is deactivated the cones will be detected with normal vision
      detectedConesLeft = DetectCone::simConeDetectorBox(m_leftCones, locationCopy, headingCopy, m_detectRange, m_detectWidth);
      detectedConesRight = DetectCone::simConeDetectorBox(m_rightCones, locationCopy, headingCopy, m_detectRange, m_detectWidth);
      detectedConesSmall = DetectCone::simConeDetectorBox(m_smallCones, locationCopy, headingCopy, m_detectRange, m_detectWidth);
      detectedConesBig = DetectCone::simConeDetectorBox(m_bigCones, locationCopy, headingCopy, m_detectRange, m_detectWidth);
    } // End of else

    // This is where the messages are sent
    Eigen::MatrixXd detectedConesLeftMat = ((detectedConesLeft.matrix()).transpose()).cast <double> ();
    Eigen::MatrixXd detectedConesRightMat = ((detectedConesRight.matrix()).transpose()).cast <double> ();
    Eigen::MatrixXd detectedConesSmallMat = ((detectedConesSmall.matrix()).transpose()).cast <double> ();
    Eigen::MatrixXd detectedConesBigMat = ((detectedConesBig.matrix()).transpose()).cast <double> ();

    std::chrono::system_clock::time_point tp = std::chrono::system_clock::now();
    cluon::data::TimeStamp sampleTime = cluon::time::convert(tp);
    opendlv::logic::perception::ObjectProperty cones;
    std::string property = std::to_string(detectedConesLeftMat.cols()+detectedConesRightMat.cols()+detectedConesSmallMat.cols()+detectedConesBigMat.cols());
    cones.property(property);
    cones.objectId(m_sendId);
    m_od4.send(cones, sampleTime, m_senderStamp);
    //std::cout<<"DetectCone sends: " <<"number of cones: "<<property<<" frame ID: "<<m_sendId<<" sampleTime: "<<cluon::time::toMicroseconds(sampleTime)<<" senderStamp: "<<m_senderStamp<<std::endl;

    int type = 1;
    auto startLeft = std::chrono::system_clock::now();
    sendMatchedContainer(detectedConesLeftMat, type, m_sendId);
    type = 2;
    sendMatchedContainer(detectedConesRightMat, type, m_sendId);
    type = 3;
    sendMatchedContainer(detectedConesSmallMat, type, m_sendId);
    type = 4;
    sendMatchedContainer(detectedConesBigMat, type, m_sendId);
    //std::cout<<"Sending with ID: "<<m_sendId<<"\n";
    int rndmId = rand();
    while (m_sendId == rndmId){rndmId = rand();}
    m_sendId = rndmId;
    auto finishRight = std::chrono::system_clock::now();
    auto timeSend = std::chrono::duration_cast<std::chrono::microseconds>(finishRight - startLeft);
    //std::cout << "sendTime:" << timeSend.count() << std::endl;
} // End of body


void DetectCone::setUp()
{
  // Starting position and heading are set in the configuration
  m_location.resize(1,2);
  m_location << m_startX,m_startY;
  m_heading = m_startHeading;
  DetectCone::readMap(m_filename);
} // End of setUp


void DetectCone::tearDown()
{
}

void DetectCone::readMap(std::string filename)
{
  int leftCounter = 0;
  int rightCounter = 0;
  int smallCounter = 0;
  int bigCounter = 0;

  std::string line, word;
  std::string const HOME = "/opt/opendlv.data/";
  std::string infile = HOME + filename;

  std::ifstream file(infile, std::ifstream::in);

  if(file.is_open())
  {
    while(getline(file,line))
    {
      std::stringstream strstr(line);

      getline(strstr,word,',');
      getline(strstr,word,',');
      getline(strstr,word,',');

      if(word.compare("1") == 0){leftCounter = leftCounter+1;}
      else if(word.compare("2") == 0){rightCounter = rightCounter+1;}
      else if(word.compare("3") == 0){smallCounter = smallCounter+1;}
      else if(word.compare("4") == 0){bigCounter = bigCounter+1;}
      else{std::cout << "ERROR in DetectCone::simDetectCone while counting types. Not a valid cone type." << std::endl;}
    } // End of while

    file.close();
  } // End of if

  Eigen::ArrayXXf tmpLeftCones(leftCounter,2);
  Eigen::ArrayXXf tmpRightCones(rightCounter,2);
  Eigen::ArrayXXf tmpSmallCones(smallCounter,2);
  Eigen::ArrayXXf tmpBigCones(bigCounter,2);
  float x, y;
  leftCounter = 0;
  rightCounter = 0;
  smallCounter = 0;
  bigCounter = 0;
  std::ifstream myFile(infile, std::ifstream::in);

  if(myFile.is_open())
  {
    while(getline(myFile,line))
    {
      std::stringstream strstr(line);

      getline(strstr,word,',');
      x = std::stof(word);
      getline(strstr,word,',');
      y = std::stof(word);

      getline(strstr,word,',');

      if(word.compare("1") == 0)
      {
        tmpLeftCones(leftCounter,0) = x;
        tmpLeftCones(leftCounter,1) = y;
        leftCounter = leftCounter+1;
      }
      else if(word.compare("2") == 0)
      {
        tmpRightCones(rightCounter,0) = x;
        tmpRightCones(rightCounter,1) = y;
        rightCounter = rightCounter+1;
      }
      else if(word.compare("3") == 0)
      {
        tmpSmallCones(smallCounter,0) = x;
        tmpSmallCones(smallCounter,1) = y;
        smallCounter = smallCounter+1;
      }
      else if(word.compare("4") == 0)
      {
        tmpBigCones(bigCounter,0) = x;
        tmpBigCones(bigCounter,1) = y;
        bigCounter = bigCounter+1;
      }
      else{std::cout << "ERROR in DetectCone::simDetectCone while storing cones. Not a valid cone type." << std::endl;}
    } // End of while

    myFile.close();
  } // End of if

  m_leftCones = tmpLeftCones;
  m_rightCones = tmpRightCones;
  m_smallCones = tmpSmallCones;
  m_bigCones = tmpBigCones;
} // End of readMap


Eigen::ArrayXXf DetectCone::simConeDetectorBox(Eigen::ArrayXXf globalMap, Eigen::ArrayXXf location, float heading, float detectRange, float detectWidth)
{
  // Input: Positions of cones and vehicle, heading angle, detection ranges forward and to the side
  // Output: Local coordinates of the cones within the specified area

  int nCones = globalMap.rows();

  Eigen::MatrixXf rotationMatrix(2,2);
  rotationMatrix << std::cos(-heading),-std::sin(-heading),
                    std::sin(-heading),std::cos(-heading);

  Eigen::ArrayXXf tmpLocation(nCones,2);
  (tmpLocation.col(0)).fill(location(0));
  (tmpLocation.col(1)).fill(location(1));

  Eigen::ArrayXXf localMap = ((rotationMatrix*(((globalMap-tmpLocation).matrix()).transpose())).transpose()).array();

  Eigen::ArrayXXf detectedCones(nCones,2);
  bool inLongitudinalInterval, inLateralInterval;
  int nFound = 0;
  for(int i = 0; i < nCones; i = i+1)
  {
    inLongitudinalInterval = localMap(i,0) < detectRange && localMap(i,0) >= 0;
    inLateralInterval = localMap(i,1) >= -detectWidth/2 && localMap(i,1) <= detectWidth/2;

    if(inLongitudinalInterval && inLateralInterval)
    {
      detectedCones.row(nFound) = localMap.row(i);
      nFound = nFound+1;
    } // End of if
  } // End of for

Eigen::ArrayXXf detectedConesFinal = detectedCones.topRows(nFound);

return detectedConesFinal;
} // End of simConeDetectorBox


Eigen::ArrayXXf DetectCone::simConeDetectorSlam(Eigen::ArrayXXf globalMap, Eigen::ArrayXXf location, float heading, int nConesInFakeSlam)
{
  // Input: Positions of cones and vehicle, heading angle, detection ranges forward and to the side
  // Output: Local coordinates of the upcoming cones

  int nCones = globalMap.rows();
  Eigen::MatrixXf rotationMatrix(2,2);
  rotationMatrix << std::cos(-heading),-std::sin(-heading),
                    std::sin(-heading),std::cos(-heading);
  Eigen::ArrayXXf tmpLocation(nCones,2);
  (tmpLocation.col(0)).fill(location(0));
  (tmpLocation.col(1)).fill(location(1));

  // Convert to local coordinates
  Eigen::ArrayXXf localMap = ((rotationMatrix*(((globalMap-tmpLocation).matrix()).transpose())).transpose()).array();

  float shortestDist = std::numeric_limits<float>::infinity();
  float tmpDist;
  int closestConeIndex = -1;

  // Find the closest cone. It will be the first in the returned sequence.
  for(int i = 0; i < nCones; i = i+1)
  {
    tmpDist = ((localMap.row(i)).matrix()).norm();
    if(tmpDist < shortestDist && tmpDist > 0)
    {
      shortestDist = tmpDist;
      closestConeIndex = i;
    } // End of if
  } // End of for

  if(closestConeIndex != -1)
  {
    Eigen::VectorXi indices;

    // If more than the existing cones are requested, send all existing cones
    if(nConesInFakeSlam >= nCones)
    {
      // If the first cone is closest, no wrap-around is needed
      if(closestConeIndex == 0)
      {
        indices = Eigen::VectorXi::LinSpaced(nCones,0,nCones-1);
      }
      else
      {
        Eigen::VectorXi firstPart = Eigen::VectorXi::LinSpaced(nCones-closestConeIndex,closestConeIndex,nCones-1);
        Eigen::VectorXi secondPart = Eigen::VectorXi::LinSpaced(closestConeIndex,0,closestConeIndex-1);
        indices.resize(firstPart.size()+secondPart.size());
        indices.topRows(firstPart.size()) = firstPart;
        indices.bottomRows(secondPart.size()) = secondPart;
      } // End of else
    }
    // If the sequence should contain both the end and beginning of the track, do wrap-around
    else if(closestConeIndex + nConesInFakeSlam > nCones)
    {
      Eigen::VectorXi firstPart = Eigen::VectorXi::LinSpaced(nCones-closestConeIndex,closestConeIndex,nCones-1);
      Eigen::VectorXi secondPart = Eigen::VectorXi::LinSpaced(nConesInFakeSlam-(nCones-closestConeIndex),0,nConesInFakeSlam-(nCones-closestConeIndex)-1);
      indices.resize(firstPart.size()+secondPart.size());
      indices.topRows(firstPart.size()) = firstPart;
      indices.bottomRows(secondPart.size()) =secondPart;
    }
    // Otherwise simply take the closest and the following cones
    else
    {
      indices = Eigen::VectorXi::LinSpaced(nConesInFakeSlam,closestConeIndex,closestConeIndex+nConesInFakeSlam-1);
    }

    // Sort the cones according to the order set above
    Eigen::ArrayXXf detectedCones(indices.size(),2);
    for(int i = 0; i < indices.size(); i = i+1)
    {
      detectedCones.row(i) = localMap.row(indices(i));
    }

    // If the first cones of the track is visible, the orange cones are set as visible as well
    if(indices.minCoeff() == 0)
    {
      m_orangeVisibleInSlam = true;
    }

    return detectedCones;

  }
  // If no closest cone was found, the returned array is empty
  else
  {
    std::cout << "Error: No cone found in fake slam detection" << std::endl;
    Eigen::ArrayXXf detectedCones(0,2);

    return detectedCones;
  } // End of else
} // End of simConeDetectorSlam


void DetectCone::sendMatchedContainer(Eigen::MatrixXd cones, int type, int startID)
{
//std::cout << "New location: " << m_location << " and heading: " << m_heading << std::endl;
//std::cout << "Sending " << cones.cols() << " of type " << type << std::endl;
  std::chrono::system_clock::time_point tp;
  cluon::data::TimeStamp sampleTime;
  opendlv::logic::sensation::Point conePoint;
  for(int n = 0; n < cones.cols(); n++){

    Cartesian2Spherical(cones(0,n), cones(1,n), 0, conePoint);
    tp = std::chrono::system_clock::now();
    sampleTime = cluon::time::convert(tp);
    opendlv::logic::perception::ObjectDirection coneDirection;
    coneDirection.objectId(startID);
    coneDirection.azimuthAngle(conePoint.azimuthAngle());
    coneDirection.zenithAngle(conePoint.zenithAngle());
    m_od4.send(coneDirection, sampleTime, m_senderStamp);

    opendlv::logic::perception::ObjectDistance coneDistance;
    coneDistance.objectId(startID);
    coneDistance.distance(conePoint.distance());
    m_od4.send(coneDistance, sampleTime, m_senderStamp);

    opendlv::logic::perception::ObjectType coneType;
    coneType.objectId(startID);
    coneType.type(type);
    m_od4.send(coneType, sampleTime, m_senderStamp);
    //std::cout<<"DetectCone sends: " <<"direction: "<<conePoint.azimuthAngle()<<" distance: "<<conePoint.distance()<<" type: "<<type<< " sampleTime: "<<cluon::time::toMicroseconds(sampleTime)<<" frame ID: "<<startID<< " senderStamp: "<<m_senderStamp<<std::endl;
  } // End of for
} // End of sendMatchedContainer


void DetectCone::Cartesian2Spherical(double x, double y, double z, opendlv::logic::sensation::Point &pointInSpherical)
{
  double distance = sqrt(x*x+y*y+z*z);
  double azimuthAngle = atan2(x,y)*static_cast<double>(RAD2DEG);
  double zenithAngle = atan2(z,sqrt(x*x+y*y))*static_cast<double>(RAD2DEG);
  pointInSpherical.distance(static_cast<float>(distance));
  pointInSpherical.azimuthAngle(static_cast<float>(azimuthAngle));
  pointInSpherical.zenithAngle(static_cast<float>(zenithAngle));
} // End of Cartesian2Spherical
