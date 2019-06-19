/**********************************************************************
 * File:        abbyyenderer.cpp
 * Description: Simple API for calling tesseract.
 * Author:      Ray Smith (original code from baseapi.cpp)
 * Author:      Stefan Weil (moved to separate file and cleaned code)
 *
 * (C) Copyright 2006, Google Inc.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 ** http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 *
 **********************************************************************/

#include <fstream>
#include <locale>     // for std::locale::classic
#include <memory>     // for std::unique_ptr
#include <sstream>
#include <algorithm>    // for std::stringstream
#include <opencv2/opencv.hpp>
#include "baseapi.h"  // for TessBaseAPI
#ifdef _WIN32
# include "host.h"    // windows.h for MultiByteToWideChar, ...
#endif
#include "renderer.h"
#include "tesseractclass.h"  // for Tesseract

namespace tesseract {

/**
 * Gets the block orientation at the current iterator position.
 */
static tesseract::Orientation GetBlockTextOrientation(const PageIterator* it) {
  tesseract::Orientation orientation;
  tesseract::WritingDirection writing_direction;
  tesseract::TextlineOrder textline_order;
  float deskew_angle;
  it->Orientation(&orientation, &writing_direction, &textline_order,
                  &deskew_angle);
  return orientation;
}

/**
 * Fits a line to the baseline at the given level, and appends its coefficients
 * to the Abbyy string.
 * NOTE: The Abbyy spec is unclear on how to specify baseline coefficients for
 * rotated textlines. For this reason, on textlines that are not upright, this
 * method currently only inserts a 'textangle' property to indicate the rotation
 * direction and does not add any baseline information to the abbyy string.
 */
// static void AddBaselineCoordsToAbbyy(const PageIterator* it,
//                                     PageIteratorLevel level,
//                                     std::stringstream& abbyy_str) {
//   tesseract::Orientation orientation = GetBlockTextOrientation(it);
//   if (orientation != ORIENTATION_PAGE_UP) {
//     abbyy_str << "; textangle " << 360 - orientation * 90;
//     return;
//   }

//   int left, top, right, bottom;
//   it->BoundingBox(level, &left, &top, &right, &bottom);

//   // Try to get the baseline coordinates at this level.
//   int x1, y1, x2, y2;
//   if (!it->Baseline(level, &x1, &y1, &x2, &y2)) return;
//   // Following the description of this field of the Abbyy spec, we convert the
//   // baseline coordinates so that "the bottom left of the bounding box is the
//   // origin".
//   x1 -= left;
//   x2 -= left;
//   y1 -= bottom;
//   y2 -= bottom;

//   // Now fit a line through the points so we can extract coefficients for the
//   // equation:  y = p1 x + p0
//   if (x1 == x2) {
//     // Problem computing the polynomial coefficients.
//     return;
//   }
//   double p1 = (y2 - y1) / static_cast<double>(x2 - x1);
//   double p0 = y1 - p1 * x1;

//   abbyy_str << "; baseline " << round(p1 * 1000.0) / 1000.0 << " "
//            << round(p0 * 1000.0) / 1000.0;
// }

static void AddBoxToAbbyy(const ResultIterator* it, PageIteratorLevel level,
                         std::stringstream& abbyy_str) {
  int left, top, right, bottom;
  it->BoundingBox(level, &left, &top, &right, &bottom);
  // This is the only place we use double quotes instead of single quotes,
  // but it may too late to change for consistency
  abbyy_str << "left='" 
            << left 
            << "' top='" 
            << top 
            << "' right='" 
            << right 
            << "' bottom='"
            << bottom;
  // Add baseline coordinates & heights for textlines only.
  // if (level == RIL_TEXTLINE) {
  //   AddBaselineCoordsToAbbyy(it, level, abbyy_str);
  //   // add custom height measures
  //   float row_height, descenders, ascenders;  // row attributes
  //   it->RowAttributes(&row_height, &descenders, &ascenders);
  //   // TODO(rays): Do we want to limit these to a single decimal place?
  //   abbyy_str << "; x_size " << row_height << "; x_descenders " << -descenders
  //            << "; x_ascenders " << ascenders;
  // }
  abbyy_str << "'>";
}

/**
 * Make a HTML-formatted string with Abbyy markup from the internal
 * data structures.
 * page_number is 0-based but will appear in the output as 1-based.
 * Image name/input_file_ can be set by SetInputName before calling
 * GetAbbyyText
 * STL removed from original patch submission and refactored by rays.
 * Returned string must be freed with the delete [] operator.
 */
char* TessBaseAPI::GetAbbyyText(int page_number) {
  return GetAbbyyText(nullptr, page_number);
}

/**
 * Make a HTML-formatted string with Abbyy markup from the internal
 * data structures.
 * page_number is 0-based but will appear in the output as 1-based.
 * Image name/input_file_ can be set by SetInputName before calling
 * GetAbbyyText
 * STL removed from original patch submission and refactored by rays.
 * Returned string must be freed with the delete [] operator.
 */
char* TessBaseAPI::GetAbbyyText(ETEXT_DESC* monitor, int page_number) {
  if (tesseract_ == nullptr || (page_res_ == nullptr && Recognize(monitor) < 0))
    return nullptr;

  int lcnt = 1, bcnt = 1, pcnt = 1, wcnt = 1, scnt = 1, tcnt = 1, gcnt = 1;
  int page_id = page_number + 1;  // Abbyy uses 1-based page numbers.
  bool para_is_ltr = true;        // Default direction is LTR
  const char* paragraph_lang = nullptr;
  bool font_info = false;
  bool abbyy_boxes = false;
  bool abbyy_debug = false;
  GetBoolVariable("abbyy_font_info", &font_info);
  GetBoolVariable("abbyy_char_boxes", &abbyy_boxes);
  GetBoolVariable("abbyy_debug", &abbyy_debug);

  if (input_file_ == nullptr) SetInputName(nullptr);

#ifdef _WIN32
  // convert input name from ANSI encoding to utf-8
  int str16_len =
      MultiByteToWideChar(CP_ACP, 0, input_file_->string(), -1, nullptr, 0);
  wchar_t* uni16_str = new WCHAR[str16_len];
  str16_len = MultiByteToWideChar(CP_ACP, 0, input_file_->string(), -1,
                                  uni16_str, str16_len);
  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, uni16_str, str16_len, nullptr,
                                     0, nullptr, nullptr);
  char* utf8_str = new char[utf8_len];
  WideCharToMultiByte(CP_UTF8, 0, uni16_str, str16_len, utf8_str, utf8_len,
                      nullptr, nullptr);
  *input_file_ = utf8_str;
  delete[] uni16_str;
  delete[] utf8_str;
#endif

  std::stringstream abbyy_str;
  // Use "C" locale (needed for double values x_size and x_descenders).
  abbyy_str.imbue(std::locale::classic());
  // Use 8 digits for double values.
  abbyy_str.precision(8);
  abbyy_str << "  <div class='page'";
  abbyy_str << " id='"
           << "page_" << page_id << "'";
  abbyy_str << " filename='";
  Joint tables_joints;
  std::vector<std::vector<int>> x_coords, y_coords;
  if (input_file_) {
    abbyy_str << std::string(HOcrEscape(input_file_->string()).c_str());
    tables_joints = ExtractTableJoints(std::string(HOcrEscape(input_file_->string()).c_str()));
    for (size_t i = 0; i < tables_joints.size(); i++) {
        std::vector<int> x, y;
        for (int j = int(tables_joints[i].size()) - 1; j > -1; j--) {
            
            for (size_t l = 0; l < tables_joints[i][j].size(); l++) {
               bool stop = false;
                for (size_t k = 0; k < x.size(); k++) {
                    if (abs(x[k] - tables_joints[i][j][l].x) < 3) {
                        stop = true;
                        break;
                    }
                }
                if (stop)
                    break;
                x.push_back(tables_joints[i][j][l].x);
            }
        }
        for (int j = int(tables_joints[i].size()) - 1; j > -1; j--) {
            bool stop = false;
            for (size_t l = 0; l < tables_joints[i][j].size(); l++) {
                for (size_t k = 0; k < y.size(); k++) {
                    if (abs(y[k] - tables_joints[i][j][l].y) < 3) {
                        stop = true;
                        break;
                    }
                }
                if (stop)
                    break;
                y.push_back(tables_joints[i][j][l].y);
            }
        }
        std::sort(x.begin(), x.end());
        std::sort(y.begin(), y.end());
        x_coords.push_back(x);
        y_coords.push_back(y);
    }

    // Debug x_coords, y_coords
    if (abbyy_debug) {
      cv::Mat src = cv::imread(std::string(HOcrEscape(input_file_->string()).c_str()));
      for (size_t i = 0; i < x_coords.size(); i++) {
        cv::Mat clone = src.clone();
        for (size_t j = 0; j < x_coords[i].size(); j++) {
          cv::line(clone, cv::Point(x_coords[i][j], 0), cv::Point(x_coords[i][j], src.rows - 1), cv::Scalar(0, 0, 255), 3);
        }
        for (size_t j = 0; j < y_coords[i].size(); j++) {
          cv::line(clone, cv::Point(0, y_coords[i][j]), cv::Point(src.cols - 1, y_coords[i][j]), cv::Scalar(0, 0, 255), 3);
        }
        cv::imshow("debug", clone);
        cv::waitKey(0);
        cv::destroyAllWindows();
      }
    }
    // End debug
        abbyy_str << "' left='" 
              << rect_left_ 
              << "' top='" 
              << rect_top_ 
              << "' width='"
              << rect_width_ 
              << "' height='" 
              << rect_height_ 
              << "' ppageno='" 
              << page_number
              << "'>\n";

    int prev_col = 0;
    int prev_row = 0;
    int prev_table_idx = -1;
    int cur_table_idx = -1;
    std::unique_ptr<ResultIterator> res_it(GetIterator());
    while (!res_it->Empty(RIL_BLOCK)) {
      if (res_it->Empty(RIL_WORD)) {
        res_it->Next(RIL_WORD);
        continue;
      }

      if (std::string(res_it->GetUTF8Text(RIL_WORD)).find_first_not_of(" \t\n\v\f\r") == std::string::npos) {
        res_it->Next(RIL_WORD);
        continue;
      }
    
      // Open any new block/paragraph/textline.
      if (res_it->IsAtBeginningOf(RIL_BLOCK)) {
        para_is_ltr = true;  // reset to default direction
        abbyy_str << "   <div class='block'"
                << " id='"
                << "block_" << page_id << "_" << bcnt << "' ";
        AddBoxToAbbyy(res_it.get(), RIL_BLOCK, abbyy_str);
      }
      
      int left, top, right, bottom, x, y;
      res_it->BoundingBox(RIL_WORD, &left, &top, &right, &bottom);

      x = (left + right) / 2.0;
      y = (top + bottom) / 2.0;
      bool in_table = false;
      for (size_t n = 0; n < tables_joints.size(); n++)
          if (IsPointInsideTable(x, y, tables_joints[n])) {
              cur_table_idx = n;
              in_table = true;
              break;
          }
      if (!in_table) {
          cur_table_idx = -1;
      }
      if (prev_table_idx != cur_table_idx) {
          abbyy_str << "\n    </table>";
          abbyy_str << "\n     </tbody>";
          prev_row = 0;
          prev_col = 0;
      }
      if (cur_table_idx != -1) {
          if (cur_table_idx != prev_table_idx) {
              abbyy_str << "\n    <table>";
              abbyy_str << "\n     <tbody>";
          }

          int cur_row = 0;
          for (size_t i = 1; i < y_coords[cur_table_idx].size(); i++) {
              if (abbyy_debug)
                std::cout << "cur_table_idx = " << cur_table_idx << " | text = " << res_it->GetUTF8Text(RIL_WORD)
                          << " | y = " << y << " | y_coords[cur_table_idx][i] = " << y_coords[cur_table_idx][i]
                          << std::endl;
              if (y < y_coords[cur_table_idx][i]) {
                  cur_row = i;
                  if (abbyy_debug)
                    std::cout << "prev_row = " << prev_row << "cur_row = " << cur_row << std::endl;
                  if (prev_row < cur_row) {
                    if (prev_row != 0) {
                      abbyy_str << "\n      </tr>";
                      prev_col = 0;
                    }
                    abbyy_str << "\n      <tr>";
                    prev_row = cur_row;
                  }
                  break;
              }
          }
          int cur_col = 0;
          for (size_t i = 1; i < x_coords[cur_table_idx].size(); i++) {
              if (abbyy_debug)
                std::cout << "cur_table_idx = " << cur_table_idx << " | text = " << res_it->GetUTF8Text(RIL_WORD)
                          << " | x = " << x << " | x_coords[cur_table_idx][i] = " << x_coords[cur_table_idx][i]
                          << std::endl;
                        
              if (x < x_coords[cur_table_idx][i]) {
                  cur_col = i;
                  if (abbyy_debug)
                    std::cout << "prev_col = " << prev_col << "cur_col = " << cur_col << std::endl;
                  if (prev_col < cur_col) {
                    if (prev_col != 0) {
                      abbyy_str << "\n       </td>";
                    }
                    abbyy_str << "\n       <td>";
                    prev_col = cur_col;
                  }
                  break;
              }
          }
          bool bold, italic, underlined, monospace, serif, smallcaps;
          int pointsize, font_id;
          const char* font_name;
          font_name =
              res_it->WordFontAttributes(&bold, &italic, &underlined, &monospace,
                                    &serif, &smallcaps, &pointsize, &font_id);
          abbyy_str << "\n        <span "
                    << "wordconfidence='"
                    << static_cast<int>(res_it->Confidence(RIL_WORD))
                    << "' left='"
                    << left
                    << "' top='"
                    << top
                    << "' right='"
                    << right
                    << "' bottom='"
                    << bottom
                    << "' wordfirst='"
                    << static_cast<int>(res_it->IsAtBeginningOf(RIL_TEXTLINE))
                    << "' wordfromdictionary='"
                    << static_cast<int>(res_it->WordIsFromDictionary())
                    << "' wordnumeric='"
                    << static_cast<int>(res_it->WordIsNumeric())
                    << "' fontsize='"
                    << pointsize
                    << "'>";
          if (bold) abbyy_str << "<strong>";
          if (italic) abbyy_str << "<em>";
          abbyy_str << std::string(res_it->GetUTF8Text(RIL_WORD));
          if (bold) abbyy_str << "</strong>";
          if (italic) abbyy_str << "</em>";
          abbyy_str << "</span>";        
      } else {
          if (res_it->IsAtBeginningOf(RIL_PARA)) {
            abbyy_str << "\n    <p class='paragraph'";
            para_is_ltr = res_it->ParagraphIsLtr();
            if (!para_is_ltr) {
              abbyy_str << " dir='rtl'";
            }
            abbyy_str << " id='"
                    << "par_" << page_id << "_" << pcnt << "' ";
            paragraph_lang = res_it->WordRecognitionLanguage();
            if (paragraph_lang) {
              abbyy_str << " lang='" << paragraph_lang << "'";
            }
            AddBoxToAbbyy(res_it.get(), RIL_PARA, abbyy_str);
          }
          if (res_it->IsAtBeginningOf(RIL_TEXTLINE)) {
            abbyy_str << "\n     <span class='line'"
                    << " id='"
                    << "line_" << page_id << "_" << lcnt << "' ";
            AddBoxToAbbyy(res_it.get(), RIL_TEXTLINE, abbyy_str);
          }

          // // Now, process the word...
          // std::vector<std::vector<std::pair<const char*, float>>>* rawTimestepMap =
          //     nullptr;
          // std::vector<std::vector<std::pair<const char*, float>>>* choiceMap =
          //     nullptr;
          // std::vector<std::vector<std::vector<std::pair<const char*, float>>>>*
          //     symbolMap = nullptr;
          // if (tesseract_->lstm_choice_mode) {

          //   choiceMap = res_it->GetBestLSTMSymbolChoices();
          //   symbolMap = res_it->GetSegmentedLSTMTimesteps();
          //   rawTimestepMap = res_it->GetRawLSTMTimesteps();
          // }
          abbyy_str << "\n      <span class='word' ";
                  // << " id='"
                  // << "word_" << page_id << "_" << wcnt << "' ";
          bool bold, italic, underlined, monospace, serif, smallcaps;
          int pointsize, font_id;
          const char* font_name;
          font_name =
              res_it->WordFontAttributes(&bold, &italic, &underlined, &monospace,
                                        &serif, &smallcaps, &pointsize, &font_id);
          
          abbyy_str << "wordconfidence='"
                    << static_cast<int>(res_it->Confidence(RIL_WORD))
                    << "' left='"
                    << left
                    << "' top='"
                    << top
                    << "' right='"
                    << right
                    << "' bottom='"
                    << bottom
                    << "' wordfirst='"
                    << static_cast<int>(res_it->IsAtBeginningOf(RIL_TEXTLINE));
          const char* lang = res_it->WordRecognitionLanguage();
          if (lang && (!paragraph_lang || strcmp(lang, paragraph_lang))) {
            abbyy_str << "' lang='" << lang;
          }
          abbyy_str << "' wordfromdictionary='"
                    << static_cast<int>(res_it->WordIsFromDictionary())
                    << "' wordnumeric='"
                    << static_cast<int>(res_it->WordIsNumeric());
          if (font_info) {
            if (font_name) {
              abbyy_str << "' font_name='" << HOcrEscape(font_name).c_str();
            }
          }
          abbyy_str << "' fontsize='"
                    << pointsize
                    << "'>";
          if (bold) abbyy_str << "<strong>";
          if (italic) abbyy_str << "<em>";

          abbyy_str << std::string(res_it->GetUTF8Text(RIL_WORD));
          if (bold) abbyy_str << "</strong>";
          if (italic) abbyy_str << "</em>";
          abbyy_str << "</span>";
          
          
          
          switch (res_it->WordDirection()) {
            // Only emit direction if different from current paragraph direction
            case DIR_LEFT_TO_RIGHT:
              if (!para_is_ltr) abbyy_str << " dir='ltr'";
              break;
            case DIR_RIGHT_TO_LEFT:
              if (para_is_ltr) abbyy_str << " dir='rtl'";
              break;
            case DIR_MIX:
            case DIR_NEUTRAL:
            default:  // Do nothing.
              break;
          }
      }

      prev_table_idx = cur_table_idx;

      bool last_word_in_line = res_it->IsAtFinalElement(RIL_TEXTLINE, RIL_WORD);
      bool last_word_in_para = res_it->IsAtFinalElement(RIL_PARA, RIL_WORD);
      bool last_word_in_block = res_it->IsAtFinalElement(RIL_BLOCK, RIL_WORD);

      do {
        const std::unique_ptr<const char[]> grapheme(
            res_it->GetUTF8Text(RIL_SYMBOL));
        // if (grapheme && grapheme[0] != 0) {
          // if (abbyy_boxes) {
          //   res_it->BoundingBox(RIL_SYMBOL, &left, &top, &right, &bottom);
          //   abbyy_str << "\n             <span class='ocrx_cinfo' title='x_bboxes "
          //            << left << " " << top << " " << right << " " << bottom
          //            << "; x_conf " << res_it->Confidence(RIL_SYMBOL) << "'>";
          // }
          // abbyy_str << HOcrEscape(grapheme.get()).c_str();
          // if (abbyy_boxes) {
          //   abbyy_str << "</span>";
          // }
        // }
        res_it->Next(RIL_SYMBOL);
      } while (!res_it->Empty(RIL_BLOCK) && !res_it->IsAtBeginningOf(RIL_WORD));

      // // If the lstm choice mode is required it is added here
      // if (tesseract_->lstm_choice_mode == 1 && rawTimestepMap != nullptr) {
      //   for (auto timestep : *rawTimestepMap) {
      //     abbyy_str << "\n       <span class='ocrx_cinfo'"
      //              << " id='"
      //              << "timestep_" << page_id << "_" << wcnt << "_" << tcnt << "'"
      //              << ">";
      //     for (std::pair<const char*, float> conf : timestep) {
      //       abbyy_str << "<span class='ocr_glyph'"
      //                << " id='"
      //                << "choice_" << page_id << "_" << wcnt << "_" << gcnt << "'"
      //                << " title='x_confs " << int(conf.second * 100) << "'>"
      //                << conf.first << "</span>";
      //       gcnt++;
      //     }
      //     abbyy_str << "</span>";
      //     tcnt++;
      //   }
      // } else if (tesseract_->lstm_choice_mode == 2 && choiceMap != nullptr) {
      //   for (auto timestep : *choiceMap) {
      //     if (timestep.size() > 0) {
      //       abbyy_str << "\n       <span class='ocrx_cinfo'"
      //                << " id='"
      //                << "lstm_choices_" << page_id << "_" << wcnt << "_" << tcnt
      //                << "'>";
      //       for (auto & j : timestep) {
      //         abbyy_str << "<span class='ocr_glyph'"
      //                  << " id='"
      //                  << "choice_" << page_id << "_" << wcnt << "_" << gcnt
      //                  << "'"
      //                  << " title='x_confs " << int(j.second * 100)
      //                  << "'>" << j.first << "</span>";
      //         gcnt++;
      //       }
      //       abbyy_str << "</span>";
      //       tcnt++;
      //     }
      //   }
      // } else if (tesseract_->lstm_choice_mode == 3 && symbolMap != nullptr) {
      //   for (auto timesteps : *symbolMap) {
      //     abbyy_str << "\n       <span class='ocr_symbol'"
      //              << " id='"
      //              << "symbol_" << page_id << "_" << wcnt << "_" << scnt
      //              << "'>";
      //     for (auto timestep : timesteps) {
      //       abbyy_str << "\n        <span class='ocrx_cinfo'"
      //                << " id='"
      //                << "timestep_" << page_id << "_" << wcnt << "_" << tcnt
      //                << "'"
      //                << ">";
      //       for (std::pair<const char*, float> conf : timestep) {
      //         abbyy_str << "<span class='ocr_glyph'"
      //                  << " id='"
      //                  << "choice_" << page_id << "_" << wcnt << "_" << gcnt
      //                  << "'"
      //                  << " title='x_confs " << int(conf.second * 100) << "'>"
      //                  << conf.first << "</span>";
      //         gcnt++;
      //       }
      //       abbyy_str << "</span>";
      //       tcnt++;
      //     }
      //     abbyy_str << "</span>";
      //     scnt++;
      //   }
      // }
      // abbyy_str << "</span>";
      // tcnt = 1;
      // gcnt = 1;
      // wcnt++;
      // Close any ending block/paragraph/textline.
      if (last_word_in_line && cur_table_idx == -1) {
        abbyy_str << "\n     </span>";
        lcnt++;
      }
      if (last_word_in_para && cur_table_idx == -1) {
        abbyy_str << "\n    </p>\n";
        pcnt++;
        para_is_ltr = true;  // back to default direction
      }
      if (last_word_in_block) {
        abbyy_str << "   </div>\n";
        bcnt++;
      }
    }
    abbyy_str << "  </div>\n";
  } else {
    abbyy_str << "unknown";
  }


  const std::string& text = abbyy_str.str();
  char* result = new char[text.length() + 1];
  strcpy(result, text.c_str());
  return result;
}

/**********************************************************************
 * Abbyy Text Renderer interface implementation
 **********************************************************************/
TessAbbyyRenderer::TessAbbyyRenderer(const char* outputbase)
    : TessResultRenderer(outputbase, "abbyy") {
  font_info_ = false;
}

TessAbbyyRenderer::TessAbbyyRenderer(const char* outputbase, bool font_info)
    : TessResultRenderer(outputbase, "abbyy") {
  font_info_ = font_info;
}

bool TessAbbyyRenderer::BeginDocumentHandler() {
  AppendString(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
      "    \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" "
      "lang=\"en\">\n <head>\n  <title>");
  AppendString(title());
  AppendString(
      "</title>\n"
      "  <meta http-equiv=\"Content-Type\" content=\"text/html;"
      "charset=utf-8\"/>\n"
      "  <meta name='ocr-system' content='tesseract " PACKAGE_VERSION
      "' />\n"
      "  <meta name='ocr-capabilities' content='ocr_page ocr_carea ocr_par"
      " ocr_line ocrx_word ocrp_wconf");
  if (font_info_) AppendString(" ocrp_lang ocrp_dir ocrp_font ocrp_fsize");
  AppendString(
      "'/>\n"
      " </head>\n"
      " <body>\n");

  return true;
}

bool TessAbbyyRenderer::EndDocumentHandler() {
  AppendString(" </body>\n</html>\n");

  return true;
}

bool TessAbbyyRenderer::AddImageHandler(TessBaseAPI* api) {
  const std::unique_ptr<const char[]> abbyy(api->GetAbbyyText(imagenum()));
  if (abbyy == nullptr) return false;

  AppendString(abbyy.get());

  return true;
}

}  // namespace tesseract
