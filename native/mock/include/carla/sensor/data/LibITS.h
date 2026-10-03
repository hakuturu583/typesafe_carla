// Mirrors LibCarla ue5-dev's carla/sensor/data/LibITS.h (issue #42): the ETSI
// ITS message model of the V2X sensors (CAM, custom messages). Every struct and
// member the shim reads is here with LibCarla's name and type; of the
// enumerations, only the values the mock's synthetic messages use. LibCarla
// 0.10.0 has no such header.
#pragma once

#include <stdint.h>

#include <array>

#include "carla/rpc/CustomV2XBytes.h"

class ITSContainer {
 public:
  typedef bool OptionalValueAvailable_t;

  typedef long Latitude_t;
  typedef long Longitude_t;

  typedef enum SemiAxisLength { SemiAxisLength_unavailable = 4095 } e_SemiAxisLength;
  typedef long SemiAxisLength_t;

  typedef enum HeadingValue { HeadingValue_unavailable = 3601 } e_HeadingValue;
  typedef long HeadingValue_t;

  typedef enum HeadingConfidence {
    HeadingConfidence_equalOrWithinOneDegree = 10
  } e_HeadingConfidence;
  typedef long HeadingConfidence_t;

  typedef struct PosConfidenceEllipse {
    SemiAxisLength_t semiMajorConfidence;
    SemiAxisLength_t semiMinorConfidence;
    HeadingValue_t semiMajorOrientation;
  } PosConfidenceEllipse_t;

  typedef long AltitudeValue_t;
  typedef enum AltitudeConfidence { AltitudeConfidence_unavailable = 15 } e_AltitudeConfidence;
  typedef long AltitudeConfidence_t;

  typedef struct Altitude {
    AltitudeValue_t altitudeValue;
    AltitudeConfidence_t altitudeConfidence;
  } Altitude_t;

  typedef struct ReferencePosition {
    Latitude_t latitude;
    Longitude_t longitude;
    PosConfidenceEllipse_t positionConfidenceEllipse;
    Altitude_t altitude;
  } ReferencePosition_t;

  typedef enum StationType {
    StationType_unknown = 0,
    StationType_pedestrian = 1,
    StationType_passengerCar = 5,
    StationType_roadSideUnit = 15
  } e_StationType;
  typedef long StationType_t;

  typedef long StationID_t;

  typedef enum messageID { messageID_custom = 0, messageID_cam = 2 } e_messageID;

  typedef struct ItsPduHeader {
    long protocolVersion;
    long messageID;
    StationID_t stationID;
  } ItsPduHeader_t;

  typedef struct Heading {
    HeadingValue_t headingValue;
    HeadingConfidence_t headingConfidence;
  } Heading_t;

  typedef long SpeedValue_t;
  typedef long SpeedConfidence_t;
  typedef struct speed {
    SpeedValue_t speedValue;
    SpeedConfidence_t speedConfidence;
  } Speed_t;

  typedef enum DriveDirection {
    DriveDirection_forward = 0,
    DriveDirection_backward = 1
  } e_DriveDirection;
  typedef long DriveDirection_t;

  typedef long VehicleLengthValue_t;
  typedef enum VehicleLengthConfidenceIndication {
    VehicleLengthConfidenceIndication_unavailable = 4
  } e_VehicleLengthConfidenceIndication;
  typedef long VehicleLengthConfidenceIndication_t;
  typedef struct VehicleLength {
    VehicleLengthValue_t vehicleLengthValue;
    VehicleLengthConfidenceIndication_t vehicleLengthConfidenceIndication;
  } VehicleLength_t;

  typedef long VehicleWidth_t;

  typedef long LongitudinalAccelerationValue_t;
  typedef enum AccelerationConfidence {
    AccelerationConfidence_unavailable = 102
  } e_AccelerationConfidence;
  typedef long AccelerationConfidence_t;
  typedef struct LongitudinalAcceleration {
    LongitudinalAccelerationValue_t longitudinalAccelerationValue;
    AccelerationConfidence_t longitudinalAccelerationConfidence;
  } LongitudinalAcceleration_t;

  typedef enum CurvatureValue { CurvatureValue_unavailable = 30001 } e_CurvatureValue;
  typedef long CurvatureValue_t;
  typedef enum CurvatureConfidence { CurvatureConfidence_unavailable = 7 } e_CurvatureConfidence;
  typedef long CurvatureConfidence_t;
  typedef struct Curvature {
    CurvatureValue_t curvatureValue;
    CurvatureConfidence_t curvatureConfidence;
  } Curvature_t;

  typedef enum CurvatureCalculationMode {
    CurvatureCalculationMode_yarRateUsed = 0
  } e_CurvatureCalculationMode;
  typedef long CurvatureCalculationMode_t;

  typedef enum YawRateValue { YawRateValue_unavailable = 32767 } e_YawRateValue;
  typedef long YawRateValue_t;
  typedef enum YawRateConfidence { YawRateConfidence_unavailable = 8 } e_YawRateConfidence;
  typedef long YawRateConfidence_t;
  typedef struct YawRate {
    YawRateValue_t yawRateValue;
    YawRateConfidence_t yawRateConfidence;
  } YawRate_t;

  typedef uint8_t AccelerationControl_t;
  typedef long LanePosition_t;

  typedef long SteeringWheelAngleValue_t;
  typedef long SteeringWheelAngleConfidence_t;
  typedef struct SteeringWheelAngle {
    SteeringWheelAngleValue_t steeringWheelAngleValue;
    SteeringWheelAngleConfidence_t steeringWheelAngleConfidence;
  } SteeringWheelAngle_t;

  typedef long LateralAccelerationValue_t;
  typedef struct LateralAcceleration {
    LateralAccelerationValue_t lateralAccelerationValue;
    AccelerationConfidence_t lateralAccelerationConfidence;
  } LateralAcceleration_t;

  typedef long VerticalAccelerationValue_t;
  typedef struct VerticalAcceleration {
    VerticalAccelerationValue_t verticalAccelerationValue;
    AccelerationConfidence_t verticalAccelerationConfidence;
  } VerticalAcceleration_t;

  typedef long PerformanceClass_t;

  typedef long ProtectedZoneID_t;
  typedef ProtectedZoneID_t CenDsrcTollingZoneID_t;
  typedef struct CenDsrcTollingZone {
    Latitude_t protectedZoneLatitude;
    Longitude_t protectedZoneLongitude;
    CenDsrcTollingZoneID_t cenDsrcTollingZoneID; /* OPTIONAL */
    OptionalValueAvailable_t cenDsrcTollingZoneIDAvailable;
  } CenDsrcTollingZone_t;

  typedef enum ProtectedZoneType { ProtectedZoneType_cenDsrcTolling = 0 } e_ProtectedZoneType;
  typedef long ProtectedZoneType_t;
  typedef long TimestampIts_t;
  typedef long ProtectedZoneRadius_t;

  typedef struct ProtectedCommunicationZone {
    ProtectedZoneType_t protectedZoneType;
    TimestampIts_t expiryTime /* OPTIONAL */;
    OptionalValueAvailable_t expiryTimeAvailable;
    Latitude_t protectedZoneLatitude;
    Longitude_t protectedZoneLongitude;
    ProtectedZoneRadius_t protectedZoneRadius /* OPTIONAL */;
    OptionalValueAvailable_t protectedZoneRadiusAvailable;
    ProtectedZoneID_t protectedZoneID /* OPTIONAL */;
    OptionalValueAvailable_t protectedZoneIDAvailable;
  } ProtectedCommunicationZone_t;

  typedef struct ProtectedCommunicationZonesRSU {
    long ProtectedCommunicationZoneCount;
    std::array<ProtectedCommunicationZone_t, 16u> data; /* (SIZE(1..16)) */
  } ProtectedCommunicationZonesRSU_t;

  typedef enum VehicleRole { VehicleRole_default = 0 } e_VehicleRole;
  typedef long VehicleRole_t;

  typedef enum ExteriorLights {
    ExteriorLights_lowBeamHeadlightsOn = 0,
    ExteriorLights_highBeamHeadlightsOn = 1,
    ExteriorLights_leftTurnSignalOn = 2,
    ExteriorLights_rightTurnSignalOn = 3,
    ExteriorLights_daytimeRunningLightsOn = 4,
    ExteriorLights_reverseLightOn = 5,
    ExteriorLights_fogLightOn = 6,
    ExteriorLights_parkingLightsOn = 7
  } e_ExteriorLights;
  typedef uint8_t ExteriorLights_t;

  typedef long DeltaLatitude_t;
  typedef long DeltaLongitude_t;
  typedef long DeltaAltitude_t;
  typedef struct DeltaReferencePosition {
    DeltaLatitude_t deltaLatitude;
    DeltaLongitude_t deltaLongitude;
    DeltaAltitude_t deltaAltitude;
  } DeltaReferencePosition_t;

  typedef long PathDeltaTime_t;
  typedef struct PathPoint {
    DeltaReferencePosition_t pathPosition;
    PathDeltaTime_t pathDeltaTime{0} /* OPTIONAL */;
    bool pathDeltaTimeAvailable{false};
  } PathPoint_t;

  typedef struct PathHistory {
    long NumberOfPathPoint;
    std::array<PathPoint_t, 40u> data; /* (SIZE(0..40)) */
  } PathHistory_t;
};

class CAMContainer {
 public:
  typedef long GenerationDeltaTime_t;

  typedef struct BasicContainer {
    ITSContainer::StationType_t stationType;
    ITSContainer::ReferencePosition_t referencePosition;
  } BasicContainer_t;

  typedef enum HighFrequencyContainer_PR : long {
    HighFrequencyContainer_PR_NOTHING,
    HighFrequencyContainer_PR_basicVehicleContainerHighFrequency,
    HighFrequencyContainer_PR_rsuContainerHighFrequency
  } HighFrequencyContainer_PR;

  typedef bool OptionalStructAvailable_t;

  typedef struct BasicVehicleContainerHighFrequency {
    ITSContainer::Heading_t heading;
    ITSContainer::Speed_t speed;
    ITSContainer::DriveDirection_t driveDirection;
    ITSContainer::VehicleLength_t vehicleLength;
    ITSContainer::VehicleWidth_t vehicleWidth;
    ITSContainer::LongitudinalAcceleration_t longitudinalAcceleration;
    ITSContainer::Curvature_t curvature;
    ITSContainer::CurvatureCalculationMode_t curvatureCalculationMode;
    ITSContainer::YawRate_t yawRate;

    OptionalStructAvailable_t accelerationControlAvailable;
    ITSContainer::AccelerationControl_t accelerationControl /* OPTIONAL */;

    OptionalStructAvailable_t lanePositionAvailable;
    ITSContainer::LanePosition_t lanePosition /* OPTIONAL */;

    OptionalStructAvailable_t steeringWheelAngleAvailable;
    ITSContainer::SteeringWheelAngle_t steeringWheelAngle /* OPTIONAL */;

    OptionalStructAvailable_t lateralAccelerationAvailable;
    ITSContainer::LateralAcceleration_t lateralAcceleration /* OPTIONAL */;

    OptionalStructAvailable_t verticalAccelerationAvailable;
    ITSContainer::VerticalAcceleration_t verticalAcceleration /* OPTIONAL */;

    OptionalStructAvailable_t performanceClassAvailable;
    ITSContainer::PerformanceClass_t performanceClass /* OPTIONAL */;

    OptionalStructAvailable_t cenDsrcTollingZoneAvailable;
    ITSContainer::CenDsrcTollingZone_t cenDsrcTollingZone /* OPTIONAL */;
  } BasicVehicleContainerHighFrequency_t;

  typedef struct RSUContainerHighFrequency {
    ITSContainer::ProtectedCommunicationZonesRSU_t protectedCommunicationZonesRSU;
  } RSUContainerHighFrequency_t;

  typedef struct HighFrequencyContainer {
    HighFrequencyContainer_PR present;
    BasicVehicleContainerHighFrequency_t basicVehicleContainerHighFrequency;
    RSUContainerHighFrequency_t rsuContainerHighFrequency;
  } HighFrequencyContainer_t;

  typedef enum LowFrequencyContainer_PR : long {
    LowFrequencyContainer_PR_NOTHING,
    LowFrequencyContainer_PR_basicVehicleContainerLowFrequency,
  } LowFrequencyContainer_PR;

  typedef struct BasicVehicleContainerLowFrequency {
    ITSContainer::VehicleRole_t vehicleRole;
    ITSContainer::ExteriorLights_t exteriorLights;
    ITSContainer::PathHistory_t pathHistory;
  } BasicVehicleContainerLowFrequency_t;

  typedef struct LowFrequencyContainer {
    LowFrequencyContainer_PR present;
    BasicVehicleContainerLowFrequency_t basicVehicleContainerLowFrequency;
  } LowFrequencyContainer_t;

  typedef struct CamParameters {
    BasicContainer_t basicContainer;
    HighFrequencyContainer_t highFrequencyContainer;
    LowFrequencyContainer_t lowFrequencyContainer; /* OPTIONAL */
  } CamParameters_t;

  typedef struct CoopAwareness {
    GenerationDeltaTime_t generationDeltaTime;
    CamParameters_t camParameters;
  } CoopAwareness_t;
};

typedef struct CAM {
  ITSContainer::ItsPduHeader_t header;
  CAMContainer::CoopAwareness_t cam;
} CAM_t;

typedef struct CustomV2XM {
  ITSContainer::ItsPduHeader_t header;
  carla::rpc::CustomV2XBytes data;
} CustomV2XM_t;
