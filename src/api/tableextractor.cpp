#include <iostream>
#include <sstream>
#include <opencv2/opencv.hpp>
#include "baseapi.h" // for TessBaseAPI

/**
 * Extract table joints using image processing.
 * Additional denoising is required.
 * Only work for Lattice-type tables (with full borders)
 * Does not work for Stream-type tables (without full borders)
 */

namespace tesseract {

Joint TessBaseAPI::ExtractTableJoints(std::string filename) {
    // Load source image
    // std::string filename(this->GetInputName());
    cv::Mat src = cv::imread(filename);

    // Check if image can be loaded
    if (!src.data)
        std::cerr << "OpenCV Error: Cannot load image!" << std::endl;

    // Transform source image to gray if it is not
    cv::Mat gray;

    if (src.channels() == 3) {
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = src;
    }

    // Apply adaptiveThreshold at the bitwise_not of gray, notice the ~ symbol
    cv::Mat bw;
    
    cv::adaptiveThreshold(~gray, bw, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY, 15, -2);

    // Create the images that will use to extract the horizontal and vertical lines
    cv::Mat horizontal = bw.clone();
    cv::Mat vertical = bw.clone();

    int scale = 30; // play with this variable in order to increase/decrease the amount of lines to be detected

    // Specify size on horizontal axis
    int horizontalsize = horizontal.cols / scale;

    // Create structure element for extracting horizontal lines through morphology operations
    cv::Mat horizontalStructure = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(horizontalsize, 1));

    // Apply morphology operations
    cv::erode(horizontal, horizontal, horizontalStructure, cv::Point(-1, -1));
    cv::dilate(horizontal, horizontal, horizontalStructure, cv::Point(-1, -1));

    // Specify size on vertical axis
    int verticalsize = vertical.rows / scale;

    // Create structure element for extracting vertical lines through morphology operations
    cv::Mat verticalStructure = cv::getStructuringElement(cv::MORPH_RECT, cv::Size( 1,verticalsize));

    // Apply morphology operations
    cv::erode(vertical, vertical, verticalStructure, cv::Point(-1, -1));
    cv::dilate(vertical, vertical, verticalStructure, cv::Point(-1, -1));

    cv::Mat mask = horizontal + vertical;

    cv::Mat joints;
    bitwise_and(horizontal, vertical, joints);

    std::vector<cv::Vec4i> hierarchy;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE, cv::Point(0, 0));

    cv::Mat src_clone = src.clone();
    // cv::drawContours(src_clone, contours, -1, cv::Scalar(0, 0, 255), 8);
    // cv::imshow("contours", src_clone);
    // cv::waitKey(0);

    std::vector<std::vector<cv::Point>> contours_poly(contours.size());
    std::vector<cv::Rect> boundRect(contours.size());
    std::vector<cv::Mat> rois;

    Joint tables_joints;

    for (size_t i = 0; i < contours.size(); i++) {
        // Find the areas of each contour
        double area = cv::contourArea(contours[i]);

        // Filter individual lines of blobs that might exist and they do not represent a table
        if (area < 50) 
            continue;

        cv::approxPolyDP(cv::Mat(contours[i]), contours_poly[i], 3, true);
        boundRect[i] = cv::boundingRect(cv::Mat(contours_poly[i])); // Find the smallest bounding rectangle of the image

        // Find number of joints that each table has
        cv::Mat roi = joints(boundRect[i]);
        cv::Point offset;
        cv::Size wholesize;
        roi.locateROI(wholesize, offset);;

        std::vector<std::vector<cv::Point>> joints_contours;
        cv::findContours(roi, joints_contours, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);

        // If the number of joints is less than 5, most likely it is not a table
        if (joints_contours.size() <= 4)
            continue;

        for (size_t j = 0; j < joints_contours.size(); j++) {
            for (size_t k = 0; k < joints_contours[j].size(); k++) {
                joints_contours[j][k].x = joints_contours[j][k].x + offset.x;
                joints_contours[j][k].y = joints_contours[j][k].y + offset.y;
            }
        }
        tables_joints.push_back(joints_contours);

        // // Debug table joints
        // cv::Mat clone = src.clone();
        // cv::drawContours(clone, joints_contours, -1, cv::Scalar(0, 0, 255), 8);
        // cv::namedWindow("table_joints", cv::WINDOW_NORMAL);
        // cv::imshow("table_joints", clone);
        // cv::waitKey(0);
        // // End debug
    }

    return tables_joints;
// return NULL;
    // return Joint(NULL);

}

bool TessBaseAPI::IsPointInsideTable(int x, int y, std::vector<std::vector<cv::Point>> table) {
    return IsPointInsideTable(cv::Point(x, y), table);
}

bool TessBaseAPI::IsPointInsideTable(cv::Point point, std::vector<std::vector<cv::Point>> table) {
    std::vector<cv::Point> new_table;
    for (std::vector<std::vector<cv::Point>>::const_iterator it = table.begin(); it != table.end(); ++it) {
        new_table.insert(new_table.end(), it->begin(), it->end());
    }
    cv::Rect boundRect = cv::boundingRect(new_table);
    bool result = boundRect.contains(point);
    return result;
    
}

}
