#include "SimpleEstimator.h"
#include <algorithm>

//NeedToBeRemoved
#include <sstream>


namespace IMU
{
SimpleEstimator::SimpleEstimator(ORB_SLAM2::System* system)
	:mSystemPtr(system),
	mTrackState(NotReady),
	mTrackPose(cv::Mat::zeros(4, 4, CV_32F)),
	mTranformR(cv::Mat::zeros(4, 4, CV_32F)),
	mTrackT(0),
	mScale(0)
{
}

SimpleEstimator::~SimpleEstimator()
{
	for (auto i : mIMUFrameV)
	{
		delete i;
	}
	mIMUFrameV.clear();
	while (mIMUDataQ.size() > 0)
	{
		delete mIMUDataQ.front();
		mIMUDataQ.pop();
	}
}

void SimpleEstimator::TrackMonocular(cv::Mat& im, long long timestamp)
{
	// Save pervious state
	long long pervT = mTrackT;
	auto prevPose = mTrackPose;
	auto prevState = mTrackState;
	//Log
	stringstream ss;
	string s;
	ss << "Start TrackMonocular " << timestamp;
	s = ss.str();
	LOGE(s.c_str());
	// Track
	auto tempMat = mSystemPtr->TrackMonocular(im, static_cast<double>(timestamp));
	// Log
	ss.str("");
	ss << "Finish TrackMonocular " << timestamp;
	s = ss.str();
	LOGE(s.c_str());
	{
		unique_lock<mutex> lock(sTrackMutex);
		mTrackState = static_cast<eTrackingState>(mSystemPtr->GetTrackingState());
		if (mTrackState == On)
		{
			mTrackPose = tempMat;
			mTrackT = timestamp;
			if (prevState == NotInitialized)
			{
				auto initF = FindFrame(mTrackT);
				(*initF)->mR.copyTo(mTranformR(cv::Rect(0, 0, 3, 3)));
				//Log : mTranformR
				stringstream ss;
				string s;
				ss << mTranformR;
				s = ss.str();
				LOGE(s.c_str());
			}
			mTrackPose = mTranformR * mTrackPose;
			//Log : mTrackPose
			std::stringstream ss;
			string s;
			ss << mTrackPose;
			s = ss.str();
			LOGE(s.c_str());
			if ((prevState == On || prevState == Steady))
			{
				mTrackState = Steady;
				// Update Δx1, Δx2, Δt1, Δt2
				cv::Mat temp = mTrackPose - prevPose;
				cv::Vec3f dx = cv::Vec3f(temp.col(3).rowRange(0, 3));
				if (prevState == Steady)
				{
					dx1 = dx2;
					dt1 = dt2;
					dx2 = dx;
					dt2 = mTrackT - pervT;
					ComputeScaleAndV(timestamp);
				}
				else
				{
					dx2 = dx;
					dt2 = mTrackT - pervT;
				}
			}
		}
		// Log : Track state
		std::stringstream ss;
		string s;
		ss << "Track state : " << mTrackState;
		s = ss.str();
		LOGE(s.c_str());
	}
}

//TODO: Consider IMU bias & optimizer ( Record some data and linear Optimize)
void SimpleEstimator::ComputeScaleAndV(long long timestamp)
{
	long long t1 = timestamp - dt2;
	long long t0 = t1 - dt1;
	cv::Vec3f dv1, dv;
	cv::Vec3f d1, d2;
	long long prevT = t0;
	int needToRemove = 0;
	SimpleIMUFrame* cFrame;
	for (int i = 0; i < mIMUFrameV.size(); i++)
	{
		cFrame = mIMUFrameV[i];
		if (cFrame->mTimestamp > t1)
		{
			d2 += cFrame->mDisplacement + dv * (float)(cFrame->mTimestamp - prevT) * 1e-9;
			dv += cFrame->mDVelocity;
		}
		else if (cFrame->mTimestamp > t0)
		{
			d1 += cFrame->mDisplacement + dv * (float)(cFrame->mTimestamp - prevT) * 1e-9;
			dv += cFrame->mDVelocity;
			dv1 = dv;
		}
		else
		{
			needToRemove = i;
			delete cFrame;
		}
		prevT = cFrame->mTimestamp;
	}
	mIMUFrameV.erase(mIMUFrameV.begin(), mIMUFrameV.begin() + needToRemove + 1);
	float k = (float)dt1 / dt2;
	mScale = (dv1[0] * dt1 * 1e-9 - d1[0] + k * d2[0]) / (-dx1[0] + k * dx2[0]);
	cv::Vec3f v0 = (mScale * dx1 - d1) / (float)dt1;
	mEstimatedV0 = v0 + dv;
	// Log : mScale
	stringstream ss;
	string s;
	ss << "mScale : " << mScale << "\n";
	ss << "v0 : " << v0 << "\n";
	ss << "dv1[0] : " << dv1[0] << "\n";
	ss << "k : " << k << "\n";
	ss << "d1[0] : " << d1[0] << "\n";
	ss << "d2[0] : " << d2[0] << "\n";
	ss << "dx1[0] : " << dx1[0] << "\n";
	ss << "dx2[0] : " << dx2[0] << "\n";
	s = ss.str();
	LOGE(s.c_str());
}

std::vector<SimpleIMUFrame*>::iterator SimpleEstimator::FindFrame(long long timestamp)
{
	SimpleIMUFrame temp(timestamp);
	return std::lower_bound(std::begin(mIMUFrameV), std::end(mIMUFrameV), &temp, SimpleIMUFrame::Compare);
}

cv::Mat SimpleEstimator::Estimate(long long timestamp)
{
	// Make Sure mIMUDataQ.size() greater than 1
	// Integrate
	cv::Vec3f dv;
	long long curTimestamp = mIMUDataQ.front()->mTimestamp;
	long long nextTimestamp;
	cv::Vec3f curAcc = mIMUDataQ.front()->mAcceleration;
	cv::Vec3f displacement;
	delete mIMUDataQ.front();
	mIMUDataQ.pop();
	double dt;
	while ((nextTimestamp = mIMUDataQ.front()->mTimestamp) < timestamp)
	{
		dt = (int)(nextTimestamp - curTimestamp) * 1e-9;
		dv += ((curAcc + mIMUDataQ.front()->mAcceleration) / 2) * dt;
		displacement += dv * dt;
		curAcc = mIMUDataQ.front()->mAcceleration;
		curTimestamp = nextTimestamp;
		delete mIMUDataQ.front();
		mIMUDataQ.pop();
	}
	{
		std::unique_lock<mutex> lock(sTrackMutex);
		mIMUFrameV.push_back(new SimpleIMUFrame(mIMUDataQ.front()->mR, timestamp, displacement, dv));
		if (mTrackState > NotInitialized && mTrackPose.rows == 4)
		{
			cv::Mat estimatedPose = cv::Mat::eye(4, 4, CV_32F);
			mIMUDataQ.back()->mR.copyTo(estimatedPose(cv::Rect(0, 0, 3, 3)));
			// Estimate
			cv::Vec3f velocity = mEstimatedV0;
			cv::Vec3f position(mTrackPose(cv::Rect(3, 0, 1, 3)));
			auto iter = FindFrame(mTrackT);
			long long pervT = mTrackT;
			while (++iter != mIMUFrameV.end())
			{
				position += velocity * (((*iter)->mTimestamp - pervT) * 1e-9) + (*iter)->mDisplacement;
				velocity += (*iter)->mDVelocity;
				pervT = (*iter)->mTimestamp;
			}
			// Scale
			position *= mScale;
			cv::Mat(position).copyTo(estimatedPose(cv::Rect(3, 0, 1, 3)));
			return estimatedPose;
		}
		else
		{
			return cv::Mat();
		}
	}
}
}

