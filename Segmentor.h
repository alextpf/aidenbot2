#pragma once

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/video.hpp>
#include <opencv2/imgproc.hpp>

#include "videoprocessor.h"
#include <list>

class Segmentor : public FrameProcessor
{
public:
	Segmentor();
	~Segmentor() {}

	virtual void Process(cv::Mat & input, cv::Mat & output);

	static void OnMouse(int event, int x, int y, int f, void* data);

	void SetBandWidth(unsigned int w)
	{
		m_BandWidth = w;
	}

private:
	// find ul, ur, ll, lr corners of user input
	void OrderCorners();

	// use the user-picked band to zero out the result of Canny
	void MaskCanny(cv::Mat & img);

	// is the point (in image coordiante) outside outer bound
	bool IsOutsideOuter(int x, int y, float o_l, float o_r, float o_u, float o_d);

    // is the point (in image coordiante) inside inner bound
    bool IsInsideInner( int x, int y, float i_l, float i_r, float i_u, float i_d );

    bool m_TableFound;

	// user picked 4 corners of table
	std::vector<cv::Point> m_Corners;

	// offset around the quadrilateral
	unsigned int m_BandWidth;

	// outer 4 corners
	cv::Point m_o_ul;
	cv::Point m_o_ur;
	cv::Point m_o_ll;
	cv::Point m_o_lr;

	// inner 4 corners
	cv::Point m_i_ul;
	cv::Point m_i_ur;
	cv::Point m_i_ll;
	cv::Point m_i_lr;


};