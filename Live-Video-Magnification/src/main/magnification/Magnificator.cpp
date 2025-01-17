/************************************************************************************/
/* An OpenCV/Qt based realtime application to magnify motion and color              */
/* Copyright (C) 2015  Jens Schindel <kontakt@jens-schindel.de>                     */
/*                                                                                  */
/* Based on the work of                                                             */
/*      Joseph Pan      <https://github.com/wzpan/QtEVM>                            */
/*      Nick D'Ademo    <https://github.com/nickdademo/qt-opencv-multithreaded>     */
/*                                                                                  */
/* Realtime-Video-Magnification->Magnificator.cpp                                   */
/*                                                                                  */
/* This program is free software: you can redistribute it and/or modify             */
/* it under the terms of the GNU General Public License as published by             */
/* the Free Software Foundation, either version 3 of the License, or                */
/* (at your option) any later version.                                              */
/*                                                                                  */
/* This program is distributed in the hope that it will be useful,                  */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of                   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                    */
/* GNU General Public License for more details.                                     */
/*                                                                                  */
/* You should have received a copy of the GNU General Public License                */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>.            */
/************************************************************************************/

#include "main/magnification/Magnificator.h"
#include "opencv2/opencv.hpp"
#include <opencv2/core/mat.hpp>

//using namespace cv;
////////////////////////
///Constructor /////////
////////////////////////
Magnificator::Magnificator(std::vector<cv::Mat> *pBuffer,
                           ImageProcessingFlags *imageProcFlags,
                           ImageProcessingSettings *imageProcSettings,
                           int *numFrames) :
    processingBuffer(pBuffer),
    imgProcFlags(imageProcFlags),
    imgProcSettings(imageProcSettings),
    numFrames(numFrames),
    currentFrame(0)
    {
        // Default magnification settings
        levels = 4;
        exaggeration_factor = 2.f;
        lambda = 0;
        delta = 0;
        breathMeasureOutput = 0;
    }
Magnificator::~Magnificator()
{
    clearBuffer();
}

int Magnificator::calculateMaxLevels()
{
    cv::Size s = processingBuffer->front().size();
    return calculateMaxLevels(s);
}
int Magnificator::calculateMaxLevels(QRect r)
{
    cv::Size s = cv::Size(r.width(),r.height());
    return calculateMaxLevels(s);
}
int Magnificator::calculateMaxLevels(cv::Size s)
{
    if (s.width > 5 && s.height > 5) {
        const cv::Size halved((1 + s.width) / 2, (1 + s.height) / 2);
        return 1 + calculateMaxLevels(halved);
    }
    return 0;
}

////////////////////////
///Magnification ///////
////////////////////////
/// prevAvg
void Magnificator:: colorMagnify() {
    int pBufferElements = processingBuffer->size();
    // Magnify only when processing buffer holds new images
    if(currentFrame >= pBufferElements)
        return;
    // Number of levels in pyramid
    //levels = DEFAULT_COL_MAG_LEVELS;
    levels = imgProcSettings->levels;
    cv::Mat input, output, color, filteredFrame, downSampledFrame, filteredMat;
    std::vector<cv::Mat> inputFrames, inputPyramid;

    int offset = 0;
    int pChannels;

    // Process every frame in buffer that wasn't magnified yet
    while(currentFrame < pBufferElements) {
        // Grab oldest frame from processingBuffer and delete it to save memory
        input = processingBuffer->front().clone();
        processingBuffer->erase(processingBuffer->begin());

        // Convert input image to 32bit float
        pChannels = input.channels();
        if(!(imgProcFlags->grayscaleOn || pChannels <= 2))
            input.convertTo(input, CV_32FC1);
        else
            input.convertTo(input, CV_32FC3);

        // Save input frame to add motion later
        inputFrames.push_back(input);

        /* 1. SPATIAL FILTER, BUILD GAUSS PYRAMID */
        buildGaussPyrFromImg(input, levels, inputPyramid);

        /* 2. CONCAT EVERY SMALLEST FRAME FROM PYRAMID IN ONE LARGE MAT, 1COL = 1FRAME */
        downSampledFrame = inputPyramid.at(levels-1);
        img2tempMat(downSampledFrame, downSampledMat, getOptimalBufferSize(imgProcSettings->framerate));

        // Save how many frames we've currently downsampled
        ++currentFrame;
        ++offset;
    }

    /* 3. TEMPORAL FILTER */
    idealFilter(downSampledMat, filteredMat, imgProcSettings->coLow, imgProcSettings->coHigh, imgProcSettings->framerate);

    /* 4. AMPLIFY */
    amplifyGaussian(filteredMat, filteredMat);

    // Add amplified image (color) to every frame
    for (int i = currentFrame-offset; i < currentFrame; ++i) {

        /* 5. DE-CONCAT 1COL TO DOWNSAMPLED COLOR IMAGE */
        tempMat2img(filteredMat, i, downSampledFrame.size(), filteredFrame);

        /* 6. RECONSTRUCT COLOR IMAGE FROM PYRAMID */
        buildImgFromGaussPyr(filteredFrame, levels, color, input.size());

        /* 7. ADD COLOR IMAGE TO ORIGINAL IMAGE */
        output = inputFrames.front()+color;

        // Scale output image an convert back to 8bit unsigned
        double min,max;
        minMaxLoc(output, &min, &max);
        if(!(imgProcFlags->grayscaleOn || pChannels <= 2)) {
            output.convertTo(output, CV_8UC1, 255.0/(max-min), -min * 255.0/(max-min));
        } else {
            output.convertTo(output, CV_8UC3, 255.0/(max-min), -min * 255.0/(max-min));
        }

        // Fill internal buffer with magnified image
        magnifiedBuffer.push_back(output);
        // Delete the currently processed input image
        inputFrames.erase(inputFrames.begin());
    }
}


// type2str source: https://stackoverflow.com/questions/10167534/how-to-find-out-what-type-of-a-mat-object-is-with-mattype-in-opencv
// Usage:
//            string ty =  type2str( prevFrame.type() );
//            printf("Matrix: %s %dx%d \n", ty.c_str(), prevFrame.cols, prevFrame.rows );
string type2str(int type) {
  string r;

  uchar depth = type & CV_MAT_DEPTH_MASK;
  uchar chans = 1 + (type >> CV_CN_SHIFT);

  switch ( depth ) {
    case CV_8U:  r = "8U"; break;
    case CV_8S:  r = "8S"; break;
    case CV_16U: r = "16U"; break;
    case CV_16S: r = "16S"; break;
    case CV_32S: r = "32S"; break;
    case CV_32F: r = "32F"; break;
    case CV_64F: r = "64F"; break;
    default:     r = "User"; break;
  }

  r += "C";
  r += (chans+'0');

  return r;
}


bool compareContoursPerimeter(vector<cv::Point> cont1, vector<cv::Point> cont2) { return cv::arcLength(cont1, 0) > cv::arcLength(cont2, 0); }

bool compareContoursArea(vector<cv::Point> cont1, vector<cv::Point> cont2) { return cv::contourArea(cont1) > cv::contourArea(cont2); }

int prevAvgContoursSum = 0;
int first = 1;
void Magnificator::laplaceMagnify() {
    int pBufferElements = processingBuffer->size();
    // Magnify only when processing buffer holds new images
    if(currentFrame >= pBufferElements)
        return;
    // Number of levels in pyramid
//    levels = DEFAULT_LAP_MAG_LEVELS;
    levels = imgProcSettings->levels;

    cv::Mat input, output, motion, hsvimg, labimg, newestMotion, preparedFrame, firstContours, temp;
    vector<cv::Mat> inputPyramid;
    int pChannels;

    // Process every frame in buffer that wasn't magnified yet
    while(currentFrame < pBufferElements) {
        // Grab oldest frame from processingBuffer and delete it to save memory
        input = processingBuffer->front().clone();
        if(currentFrame == 0) {
            prevFrame = input; // save first raw input as prevFrame (for motion)
        }

            else {
                processingBuffer->erase(processingBuffer->begin()); // delete oldest frame
//                cv::imshow("First", prevFrame); // NOTE using imshow does not close the program.
        }


        // Convert input image to 32bit float
        pChannels = input.channels();
        if(!(imgProcFlags->grayscaleOn || pChannels <= 2)) {
            // Convert color images to YCrCb
            input.convertTo(input, CV_32FC3, 1.0/255.0f);
            cvtColor(input, input, cv::COLOR_BGR2YCrCb);
        }
        else
            input.convertTo(input, CV_32FC1, 1.0/255.0f);

        /* 1. SPATIAL FILTER, BUILD LAPLACE PYRAMID */
        buildLaplacePyrFromImg(input, levels, inputPyramid);

        // If first frame ever, save unfiltered pyramid
        if(currentFrame == 0) {
            lowpassHi = inputPyramid;
            lowpassLo = inputPyramid;
            motionPyramid = inputPyramid;
        } else {
            /* 2. TEMPORAL FILTER EVERY LEVEL OF LAPLACE PYRAMID */
            for (int curLevel = 0; curLevel < levels; ++curLevel) {
                iirFilter(inputPyramid.at(curLevel), motionPyramid.at(curLevel), lowpassHi.at(curLevel), lowpassLo.at(curLevel),
                          imgProcSettings->coLow, imgProcSettings->coHigh);
            }

            int w = input.size().width;
            int h = input.size().height;

            // Amplification variable
            delta = imgProcSettings->coWavelength / (8.0 * (1.0 + imgProcSettings->amplification));

            // Amplification Booster for better visualization
            exaggeration_factor = DEFAULT_LAP_MAG_EXAGGERATION;

            // compute representative wavelength, lambda
            // reduces for every pyramid level
            lambda = sqrt(w*w + h*h)/3.0;

            /* 3. AMPLIFY EVERY LEVEL OF LAPLACE PYRAMID */
            for (int curLevel = levels; curLevel >= 0; --curLevel) {
                amplifyLaplacian(motionPyramid.at(curLevel), motionPyramid.at(curLevel), curLevel);
                lambda /= 2.0;
            }
        }

        // Motion is nothing up until this point
        /* 4. RECONSTRUCT MOTION IMAGE FROM PYRAMID */
        buildImgFromLaplacePyr(motionPyramid, levels, motion);



        /* 5. ATTENUATE (if not grayscale) */
        attenuate(motion, motion);

        /* 6. ADD MOTION TO ORIGINAL IMAGE */
        if(currentFrame > 0) {
            output = input+motion; // used in original
            temp = motion;
//             output = motion;
//            output = hsvimg;
        }
        else {
            output = input;
            temp = input;
        }

        // Scale output image an convert back to 8bit unsigned
        if(!(imgProcFlags->grayscaleOn || pChannels <= 2)) {
            // Convert YCrCb image back to BGR
            cvtColor(output, output, cv::COLOR_YCrCb2BGR);
            output.convertTo(output, CV_8UC3, 255.0, 1.0/255.0);
        }
        else {
            output.convertTo(output, CV_8UC1, 255.0, 1.0/255.0);
        }

        if(!(imgProcFlags->grayscaleOn || pChannels <= 2)) {
            // Convert YCrCb image back to BGR
            cvtColor(temp, temp, cv::COLOR_YCrCb2BGR);
            temp.convertTo(temp, CV_8UC3, 255.0, 1.0/255.0);
        }
        else {
            temp.convertTo(temp, CV_8UC1, 255.0, 1.0/255.0);
        }

        // detect motion between input and prevFrame. on 2nd+ frame. Then set prevFrame to input.
        // based upon https://towardsdatascience.com/image-analysis-for-beginners-creating-a-motion-detector-with-opencv-4ca6faba4b42
        if (currentFrame > 0) {
            newestMotion = temp;

            // convert prevFrame
            cvtColor(prevFrame, prevFrame, cv::COLOR_BGR2GRAY);
            cv::GaussianBlur(prevFrame, prevFrame, cv::Size(5,5), 0, 0);
            prevFrame.convertTo(prevFrame, CV_8UC1, 255.0, 1.0/255.0);

            // convert newestMotion as a gray of output
            cvtColor(temp, newestMotion, cv::COLOR_BGR2GRAY);
            cv::GaussianBlur(newestMotion, newestMotion, cv::Size(5,5), 0, 0);

            preparedFrame = prevFrame;

            // Dif between previous, raw frame and newst output frame
            cv::absdiff(prevFrame, newestMotion, preparedFrame);

            cv::Mat one = cv::Mat::ones(2, 2, CV_8UC1);

            cv::dilate(preparedFrame, preparedFrame, one, cv::Point(-1,-1), 1);

            cv::Mat threshFrame;
            cv::threshold(preparedFrame, threshFrame, 20, 255, cv::THRESH_BINARY); // 20, 255 are the thresholds.

            bitwise_not(threshFrame, threshFrame); // invert image so foreground is white, background is black. (contours detct white on black)

            // threshold frame honestly looks pretty good, if can find countours in that then do area from the tutorial, etc.
            temp = threshFrame; // Set the output to threshold frame.


            cvtColor(temp, temp, cv::COLOR_GRAY2BGR);
//            cv::imshow("BGR", output); // here it's white and black, looks pretty decent.

            // TODO: things to improve detection:
            // FIND CLUSTERS OF CONTOURS sthat move together and track them.
            // maybe do morebluring for noise reduction? as in https://docs.opencv.org/3.4/da/d0c/tutorial_bounding_rects_circles.html
            // maybe try Hull from OpenCV?

            // contours approach
            vector<vector<cv::Point>> contours;
            cv::findContours(threshFrame, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_TC89_L1);

            // init finalFrame
            cv::Mat finalFrame = cv::Mat::zeros(input.size().height, input.size().width, CV_8UC3);

            // sort in descending from largest to smallest contour (based on contour area).
            std::sort(contours.begin(), contours.end(), compareContoursArea);

            int numContours = contours.size();

            int desiredLongest = 50;
            // draw contours on cv::Mat frame.
            for (int i = 0; i < std::min(numContours, desiredLongest); i++) {
                cv::drawContours(finalFrame, contours, i, cv::Scalar(0,255,0), 2, cv::LINE_AA);
            }

            // Toggle between showing the contours or magnified image based on button
            if (imgProcSettings->MagnifiedOrContours) {
                output = finalFrame; // this is the frame after contours have been added.
            }

            // save the very first contours frame
            if (first) {
                firstContours = temp;
            } else {
                temp = finalFrame - firstContours;
            }


            // Iterate through up to the desiredLongest largest contours.
            int contoursSum = 0;

            for (size_t i = 0; i < std::min(numContours, desiredLongest); i++) {
                // Contours is a vector of contours(which are stored as point vectors)
                vector<cv::Point> pont = contours[i]; // pont is a contour defined as a vector consistuing of multiple points.
                int ySum = 0;


                for (size_t j = 0; j < pont.size(); j++) {
                    ySum += pont[j].y; // A y coord of one the the vector points.
                    // use this if want to get minimum y-value.
//                    if (pont[j].y < ySum) {
//                        ySum = pont[j].y;
//                    }
                }
                // avg y of this contour
                ySum /= pont.size();

                contoursSum += ySum;
            }

            // if only 7 contours, likely not breathing.
            if (numContours <= 7) {
                contoursSum = 0;
            } else {
                contoursSum = contoursSum / std::min(numContours, desiredLongest);
            }

//            cout << "Avg contours y-value: " << contoursSum << " # contours: " << std::min(numContours, desiredLongest) << " Contours. " << endl;


            // set initial prevavgcontourssum if first frame.
            if (currentFrame == 0) {
                prevAvgContoursSum = contoursSum;
//                prevNumContours = numContours;
            }

            // Used by processing thread to write to shared mem.
            breathMeasureOutput = contoursSum;

            prevAvgContoursSum = contoursSum;


// Example of adding text to the output video shown in the program.
//            std::string txt;
//            txt = "TEST. " + std::to_string(contoursSum) ;
//            cv::putText(output, //target image
//                        txt, //text
//                        cv::Point(10, output.rows / 3), //top-left position
//                        cv::FONT_HERSHEY_DUPLEX,
//                        1.0,
//                        CV_RGB(118, 185, 0), //font color
//                        2);
            prevFrame = input;
        }

        // Fill internal buffer with magnified image
        magnifiedBuffer.push_back(output);
        ++currentFrame;
    }
}

void Magnificator::rieszMagnify()
{
    int pBufferElements = static_cast<int>(processingBuffer->size());
    // Magnify only when processing buffer holds new images
    if(currentFrame >= pBufferElements)
        return;
    // Number of levels in pyramid
    levels = imgProcSettings->levels;


    cv::Mat buffer_in, input, magnified, output;
    std::vector<cv::Mat> channels;
    int pChannels;
    static const double PI_PERCENT = M_PI / 100.0;

    // Process every frame in buffer that wasn't magnified yet
    while(currentFrame < pBufferElements)
    {
        // Grab oldest frame from processingBuffer and delete it to save memory
        buffer_in = processingBuffer->front().clone();
        if(currentFrame > 0)
        {
            processingBuffer->erase(processingBuffer->begin());
        }

        // Convert input image to 32bit float
        pChannels = buffer_in.channels();
        if(!(imgProcFlags->grayscaleOn || pChannels <= 2))
        {
            // Convert color images to YCrCb
            buffer_in.convertTo(buffer_in, CV_32FC3, 1.0/255.0);
            cvtColor(buffer_in, buffer_in, cv::COLOR_BGR2YCrCb);
            cv::split(buffer_in, channels);
            input = channels[0];
        }
        else
        {
            buffer_in.convertTo(input, CV_32FC1, 1.0/255.0);
        }

        // If first frame ever, init pointer and init class
        if( !(curPyr && oldPyr && loCutoff && hiCutoff) )
        {
            curPyr.reset();
            oldPyr.reset();
            loCutoff.reset();
            hiCutoff.reset();
            // Pyramids
            curPyr = std::shared_ptr<RieszPyramid>(new RieszPyramid());
            oldPyr = std::shared_ptr<RieszPyramid>(new RieszPyramid());
            curPyr->init(input, levels);
            oldPyr->init(input, levels);
            // Temporal Bandpass Filters, low and highpass (Butterworth)
            loCutoff = std::shared_ptr<RieszTemporalFilter>(new RieszTemporalFilter(imgProcSettings->coLow, imgProcSettings->framerate));
            hiCutoff = std::shared_ptr<RieszTemporalFilter>(new RieszTemporalFilter(imgProcSettings->coHigh, imgProcSettings->framerate));
            loCutoff->computeCoefficients();
            hiCutoff->computeCoefficients();
        }
        else
        {
            // Check if temporal filter setting was updated
            // Update low and highpass butterworth filter coefficients if changed in GUI
            if(loCutoff->itsFrequency != imgProcSettings->coLow)
            {
                loCutoff->updateFrequency(imgProcSettings->coLow);
            }
            if(hiCutoff->itsFrequency != imgProcSettings->coHigh)
            {
                hiCutoff->updateFrequency(imgProcSettings->coHigh);
            }

            /* 1. BUILD RIESZ PYRAMID */
            curPyr->buildPyramid(input);
            /* 2. UNWRAPE PHASE TO GET HORIZ&VERTICAL / SIN&COS */
            curPyr->unwrapOrientPhase(*oldPyr);
            // 3. BANDPASS FILTER ON EACH LEVEL
            for (int lvl = 0; lvl < curPyr->numLevels-1; ++lvl) {
                loCutoff->pass(curPyr->pyrLevels[lvl].itsImagPass,
                              curPyr->pyrLevels[lvl].itsPhase,
                              oldPyr->pyrLevels[lvl].itsPhase);

                hiCutoff->pass(curPyr->pyrLevels[lvl].itsRealPass,
                              curPyr->pyrLevels[lvl].itsPhase,
                              oldPyr->pyrLevels[lvl].itsPhase);
            }
            // Shift current to prior for next iteration
            *oldPyr = *curPyr;
            // 4. AMPLIFY MOTION
            curPyr->amplify(imgProcSettings->amplification, imgProcSettings->coWavelength*PI_PERCENT);
        }

        /* 6. ADD MOTION TO ORIGINAL IMAGE */
        if(currentFrame > 0)
        {
            magnified = curPyr->collapsePyramid();
        }
        else
        {
            magnified = input;
        }

        // Scale output image and convert back to 8bit unsigned
        if(!(imgProcFlags->grayscaleOn || pChannels <= 2))
        {
            // Convert YCrCb image back to BGR
            channels[0] = magnified;
            cv::merge(channels, output);
            cvtColor(output, output, cv::COLOR_YCrCb2BGR);
            output.convertTo(output, CV_8UC3, 255.0, 1.0/255.0);
        }
        else
        {
            magnified.convertTo(output, CV_8UC1, 255.0, 1.0/255.0);
        }

        // Fill internal buffer with magnified image
        magnifiedBuffer.push_back(output);
        ++currentFrame;
    }
}

////////////////////////
///Magnified Buffer ////
////////////////////////
cv::Mat Magnificator::getFrameLast()
{
    // Take newest image
    cv::Mat img = this->magnifiedBuffer.back().clone();
    // Delete the oldest picture
    this->magnifiedBuffer.erase(magnifiedBuffer.begin());
    currentFrame = magnifiedBuffer.size();

    return img;
}

cv::Mat Magnificator::getFrameFirst()
{
    // Take oldest image
    cv::Mat img = this->magnifiedBuffer.front().clone();
    // Delete the oldest picture
    this->magnifiedBuffer.erase(magnifiedBuffer.begin());
    currentFrame = magnifiedBuffer.size();

    return img;
}

cv::Mat Magnificator::getFrameAt(int n)
{
    int mLength = magnifiedBuffer.size();
    cv::Mat img;

    if(n < mLength-1)
        img = this->magnifiedBuffer.at(n).clone();
    else {
        img = getFrameLast();
    }
    // Delete the oldest picture
    currentFrame = magnifiedBuffer.size();

    return img;
}

bool Magnificator::hasFrame()
{
    return !this->magnifiedBuffer.empty();
}

int Magnificator::getBufferSize()
{
    return magnifiedBuffer.size();
}

void Magnificator::clearBuffer()
{
    // Clear internal cache
    this->magnifiedBuffer.clear();
    this->lowpassHi.clear();
    this->lowpassLo.clear();
    this->motionPyramid.clear();
    this->downSampledMat = cv::Mat();
    this->currentFrame = 0;
    oldPyr.reset();
    curPyr.reset();
    loCutoff.reset();
    hiCutoff.reset();
}



int Magnificator::getOptimalBufferSize(int fps)
{
    // Calculate number of images needed to represent 2 seconds of film material
    unsigned int round = (unsigned int) std::max(2*fps,16);
    // Round to nearest higher power of 2
    round--;
    round |= round >> 1;
    round |= round >> 2;
    round |= round >> 4;
    round |= round >> 8;
    round |= round >> 16;
    round++;

    return round;
}

////////////////////////
///Postprocessing //////
////////////////////////
void Magnificator::amplifyLaplacian(const cv::Mat &src, cv::Mat &dst, int currentLevel)
{
    float currAlpha = (lambda/(delta*8.0) - 1.0) * exaggeration_factor;
    // Set lowpassed&downsampled image and difference image with highest resolution to 0,
    // amplify every other level
    dst = (currentLevel == levels || currentLevel == 0) ? src * 0
                                                        : src * std::min((float)imgProcSettings->amplification, currAlpha);
}

void Magnificator::attenuate(const cv::Mat &src, cv::Mat &dst)
{
    // Attenuate only if image is not grayscale
    if(src.channels() > 2)
    {
        cv::Mat planes[3];
        split(src, planes);
        planes[1] = planes[1] * imgProcSettings->chromAttenuation;
        planes[2] = planes[2] * imgProcSettings->chromAttenuation;
        merge(planes, 3, dst);
    }
}

void Magnificator::amplifyGaussian(const cv::Mat &src, cv::Mat &dst)
{
    dst = src * imgProcSettings->amplification;
}
