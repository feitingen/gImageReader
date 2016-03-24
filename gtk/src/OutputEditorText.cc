/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * OutputEditorText.cc
 * Copyright (C) 2013-2016 Sandro Mani <manisandro@gmail.com>
 *
 * gImageReader is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gImageReader is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tesseract/baseapi.h>

#include "FileDialogs.hh"
#include "OutputBuffer.hh"
#include "OutputEditorText.hh"
#include "Recognizer.hh"
#include "SourceManager.hh"
#include "SubstitutionsManager.hh"
#include "Utils.hh"

#include <cstring>
#include <fstream>


OutputEditorText::OutputEditorText()
{
	m_paneWidget = Builder("box:output");
	m_insButton = Builder("menubutton:output.insert");
	m_insImage = Builder("image:output.insert");
	m_replaceBox = Builder("box:output.findreplace");
	m_outputBox = Builder("box:output");
	m_textView = Builder("textview:output");
	m_searchEntry = Builder("entry:output.search");
	m_replaceEntry = Builder("entry:output.replace");
	m_filterKeepIfEndMark = Builder("menuitem:output.stripcrlf.keependmark");
	m_filterKeepIfQuote = Builder("menuitem:output.stripcrlf.keepquote");
	m_filterJoinHyphen = Builder("menuitem:output.stripcrlf.joinhyphen");
	m_filterJoinSpace = Builder("menuitem:output.stripcrlf.joinspace");
	m_filterKeepParagraphs = Builder("menuitem:output.stripcrlf.keepparagraphs");
	m_toggleSearchButton = Builder("button:output.findreplace");
	m_undoButton = Builder("button:output.undo");
	m_redoButton = Builder("button:output.redo");
	m_csCheckBox = Builder("checkbutton:output.matchcase");
	m_textBuffer = OutputBuffer::create();
	m_textView->set_source_buffer(m_textBuffer);
	Gtk::Button* saveButton = Builder("button:output.save");

	Glib::RefPtr<Gtk::AccelGroup> group = MAIN->getWindow()->get_accel_group();
	m_undoButton->add_accelerator("clicked", group, GDK_KEY_Z, Gdk::CONTROL_MASK, Gtk::AccelFlags(0));
	m_redoButton->add_accelerator("clicked", group, GDK_KEY_Z, Gdk::CONTROL_MASK|Gdk::SHIFT_MASK, Gtk::AccelFlags(0));
	m_toggleSearchButton->add_accelerator("clicked", group, GDK_KEY_F, Gdk::CONTROL_MASK, Gtk::AccelFlags(0));
	saveButton->add_accelerator("clicked", group, GDK_KEY_S, Gdk::CONTROL_MASK, Gtk::AccelFlags(0));

	m_insertMode = InsertMode::Append;

	m_spell.property_decode_language_codes() = true;

	CONNECT(Builder("menuitem:output.insert.append").as<Gtk::MenuItem>(), activate, [this]{ setInsertMode(InsertMode::Append, "ins_append.png"); });
	CONNECT(Builder("menuitem:output.insert.cursor").as<Gtk::MenuItem>(), activate, [this]{ setInsertMode(InsertMode::Cursor, "ins_cursor.png"); });
	CONNECT(Builder("menuitem:output.insert.replace").as<Gtk::MenuItem>(), activate, [this]{ setInsertMode(InsertMode::Replace, "ins_replace.png"); });
	CONNECT(Builder("button:output.stripcrlf").as<Gtk::Button>(), clicked, [this]{ filterBuffer(); });
	CONNECT(m_toggleSearchButton, toggled, [this]{ toggleReplaceBox(); });
	CONNECT(m_undoButton, clicked, [this]{ m_textBuffer->undo(); scrollCursorIntoView(); });
	CONNECT(m_redoButton, clicked, [this]{ m_textBuffer->redo(); scrollCursorIntoView(); });
	CONNECT(saveButton, clicked, [this]{ save(); });
	CONNECT(Builder("button:output.clear").as<Gtk::Button>(), clicked, [this]{ clear(); });
	CONNECTP(m_textBuffer, can_undo, [this]{ m_undoButton->set_sensitive(m_textBuffer->can_undo()); });
	CONNECTP(m_textBuffer, can_redo, [this]{ m_redoButton->set_sensitive(m_textBuffer->can_redo()); });
	CONNECT(m_csCheckBox, toggled, [this]{ Utils::clear_error_state(m_searchEntry); });
	CONNECT(m_searchEntry, changed, [this]{ Utils::clear_error_state(m_searchEntry); });
	CONNECT(m_searchEntry, activate, [this]{ findReplace(false, false); });
	CONNECT(m_replaceEntry, activate, [this]{ findReplace(false, true); });
	CONNECT(Builder("button:output.searchnext").as<Gtk::Button>(), clicked, [this]{ findReplace(false, false); });
	CONNECT(Builder("button:output.searchprev").as<Gtk::Button>(), clicked, [this]{ findReplace(true, false); });
	CONNECT(Builder("button:output.replace").as<Gtk::Button>(), clicked, [this]{ findReplace(false, true); });
	CONNECT(Builder("button:output.replaceall").as<Gtk::Button>(), clicked, [this]{ replaceAll(); });
	CONNECTP(Builder("fontbutton:config.settings.customoutputfont").as<Gtk::FontButton>(), font_name, [this]{ setFont(); });
	CONNECT(Builder("checkbutton:config.settings.defaultoutputfont").as<Gtk::CheckButton>(), toggled, [this]{ setFont(); });
	CONNECT(Builder("button:output.substitutions").as<Gtk::Button>(), clicked, [this]{ SubstitutionsManager::set_visible(true); });
	CONNECT(m_textView, populate_popup, [this](Gtk::Menu* menu){ completeTextViewMenu(menu); });
	CONNECTS(Builder("menuitem:output.stripcrlf.drawwhitespace").as<Gtk::CheckMenuItem>(), toggled, [this](Gtk::CheckMenuItem* item){
		m_textView->set_draw_spaces(item->get_active() ? (Gsv::DRAW_SPACES_NEWLINE|Gsv::DRAW_SPACES_TAB|Gsv::DRAW_SPACES_SPACE) : Gsv::DrawSpacesFlags(0));
	});

	// If the insert or selection mark change save the bounds either if the view is focused or the selection is non-empty
	CONNECTP(m_textBuffer, cursor_position, [this]{ m_textBuffer->save_region_bounds(m_textView->is_focus()); });
	CONNECTP(m_textBuffer, has_selection, [this]{ m_textBuffer->save_region_bounds(m_textView->is_focus()); });

	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckMenuItem>("keepdot", "menuitem:output.stripcrlf.keependmark"));
	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckMenuItem>("keepquote", "menuitem:output.stripcrlf.keepquote"));
	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckMenuItem>("joinhyphen", "menuitem:output.stripcrlf.joinhyphen"));
	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckMenuItem>("joinspace", "menuitem:output.stripcrlf.joinspace"));
	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckMenuItem>("keepparagraphs", "menuitem:output.stripcrlf.keepparagraphs"));
	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckMenuItem>("drawwhitespace", "menuitem:output.stripcrlf.drawwhitespace"));
	MAIN->getConfig()->addSetting(new SwitchSettingT<Gtk::CheckButton>("searchmatchcase", "checkbutton:output.matchcase"));
	MAIN->getConfig()->addSetting(new VarSetting<Glib::ustring>("outputdir"));

	if(MAIN->getConfig()->getSetting<VarSetting<Glib::ustring>>("outputdir")->getValue().empty()){
		MAIN->getConfig()->getSetting<VarSetting<Glib::ustring>>("outputdir")->setValue(Utils::get_documents_dir());
	}

	setFont();
}

void OutputEditorText::setFont()
{
	if(Builder("checkbutton:config.settings.defaultoutputfont").as<Gtk::CheckButton>()->get_active()){
		Builder("textview:output").as<Gtk::TextView>()->unset_font();
	}else{
		Gtk::FontButton* fontBtn = Builder("fontbutton:config.settings.customoutputfont");
		Builder("textview:output").as<Gtk::TextView>()->override_font(Pango::FontDescription(fontBtn->get_font_name()));
	}
}

void OutputEditorText::scrollCursorIntoView()
{
	m_textView->scroll_to(m_textView->get_buffer()->get_insert());
	m_textView->grab_focus();
}

void OutputEditorText::setInsertMode(InsertMode mode, const std::string& iconName)
{
	m_insertMode = mode;
	m_insImage->set(Gdk::Pixbuf::create_from_resource(std::string("/org/gnome/gimagereader/") + iconName));
}

void OutputEditorText::filterBuffer()
{
	Gtk::TextIter start, end;
	m_textBuffer->get_region_bounds(start, end);
	Glib::ustring txt = m_textBuffer->get_text(start, end);

	Utils::busyTask([this,&txt]{
		// Always remove trailing whitespace
		txt = Glib::Regex::create("\\s+\\n")->replace(txt, 0, "\\n", static_cast<Glib::RegexMatchFlags>(0));

		if(m_filterJoinHyphen->get_active()){
			txt = Glib::Regex::create("-\\s*\n\\s*")->replace(txt, 0, "", static_cast<Glib::RegexMatchFlags>(0));
		}
		Glib::ustring preChars, sucChars;
		if(m_filterKeepParagraphs->get_active()){
			preChars += "\\n"; // Keep if preceded by line break
		}
		if(m_filterKeepIfEndMark->get_active()){
			preChars += "\\.\\?!"; // Keep if preceded by end mark (.?!)
		}
		if(m_filterKeepIfQuote->get_active()){
			preChars += "'\""; // Keep if preceded by dot
			sucChars += "'\""; // Keep if succeeded by dot
		}
		if(m_filterKeepParagraphs->get_active()){
			sucChars += "\\n"; // Keep if succeeded by line break
		}
		if(!preChars.empty()){
			preChars = "([^" + preChars + "])";
		}
		if(!sucChars.empty()){
			sucChars = "(?![" + sucChars + "])";
		}
		Glib::ustring expr = preChars + "\\n" + sucChars;
		txt = Glib::Regex::create(expr)->replace(txt, 0, preChars.empty() ? " " : "\\1 ", static_cast<Glib::RegexMatchFlags>(0));

		if(m_filterJoinSpace->get_active()){
			txt = Glib::Regex::create("[ \t]+")->replace(txt, 0, " ", static_cast<Glib::RegexMatchFlags>(0));
		}
		return true;
	}, _("Stripping line breaks..."));

	start = end = m_textBuffer->insert(m_textBuffer->erase(start, end), txt);
	start.backward_chars(txt.size());
	m_textBuffer->select_range(start, end);
}

void OutputEditorText::toggleReplaceBox()
{
	m_searchEntry->set_text("");
	m_replaceEntry->set_text("");
	m_replaceBox->set_visible(m_toggleSearchButton->get_active());
}

void OutputEditorText::completeTextViewMenu(Gtk::Menu *menu)
{
	Gtk::CheckMenuItem* item = Gtk::manage(new Gtk::CheckMenuItem(_("Check spelling")));
	item->set_active(GtkSpell::Checker::get_from_text_view(*m_textView));
	CONNECT(item, toggled, [this, item]{
		if(item->get_active()) {
			setLanguage(MAIN->getRecognizer()->getSelectedLanguage());
		} else {
			setLanguage(Config::Lang());
		}
	});
	menu->prepend(*Gtk::manage(new Gtk::SeparatorMenuItem()));
	menu->prepend(*item);
	menu->show_all();
}

void OutputEditorText::findNext()
{
	findReplace(false, false);
}

void OutputEditorText::findPrev()
{
	findReplace(true, false);
}

void OutputEditorText::replaceNext()
{
	findReplace(false, true);
}

void OutputEditorText::replaceAll()
{
	MAIN->pushState(MainWindow::State::Busy, _("Replacing..."));
	Glib::ustring searchstr = m_searchEntry->get_text();
	Glib::ustring replacestr = m_replaceEntry->get_text();
	if(!m_textBuffer->replaceAll(searchstr, replacestr, m_csCheckBox->get_active())){
		Utils::set_error_state(m_searchEntry);
	}
	MAIN->popState();
}

void OutputEditorText::findReplace(bool backwards, bool replace)
{
	Utils::clear_error_state(m_searchEntry);
	Glib::ustring searchstr = m_searchEntry->get_text();
	Glib::ustring replacestr = m_replaceEntry->get_text();
	if(!m_textBuffer->findReplace(backwards, replace, m_csCheckBox->get_active(), searchstr, replacestr, m_textView)){
		Utils::set_error_state(m_searchEntry);
	}
}

void OutputEditorText::read(tesseract::TessBaseAPI &tess, ReadSessionData *data)
{
	char* text = tess.GetUTF8Text();
	bool& insertText = static_cast<TextReadSessionData*>(data)->insertText;
	Utils::runInMainThreadBlocking([&]{ addText(text, insertText); });
	delete[] text;
	insertText = true;
}

void OutputEditorText::readError(const Glib::ustring &errorMsg, ReadSessionData *data)
{
	bool& insertText = static_cast<TextReadSessionData*>(data)->insertText;
	Utils::runInMainThreadBlocking([&]{ addText(Glib::ustring::compose(_("\n[Failed to recognize page %1]\n"), errorMsg), insertText); });
	insertText = true;
}

void OutputEditorText::addText(const Glib::ustring& text, bool insert)
{
	if(insert){
		m_textBuffer->insert_at_cursor(text);
	}else{
		if(m_insertMode == InsertMode::Append){
			m_textBuffer->place_cursor(m_textBuffer->insert(m_textBuffer->end(), text));
		}else if(m_insertMode == InsertMode::Cursor){
			Gtk::TextIter start, end;
			m_textBuffer->get_region_bounds(start, end);
			m_textBuffer->place_cursor(m_textBuffer->insert(m_textBuffer->erase(start, end), text));
		}else if(m_insertMode == InsertMode::Replace){
			m_textBuffer->place_cursor(m_textBuffer->insert(m_textBuffer->erase(m_textBuffer->begin(), m_textBuffer->end()), text));
		}
	}
	MAIN->setOutputPaneVisible(true);
}

bool OutputEditorText::save(const std::string& filename)
{
	std::string outname = filename;
	if(outname.empty()){
		std::vector<Source*> sources = MAIN->getSourceManager()->getSelectedSources();
		std::string ext, base;
		std::string name = !sources.empty() ? sources.front()->displayname : _("output");
		Utils::get_filename_parts(name, base, ext);
		outname = Glib::build_filename(MAIN->getConfig()->getSetting<VarSetting<Glib::ustring>>("outputdir")->getValue(), base + ".txt");

		FileDialogs::FileFilter filter = {_("Text Files"), {"text/plain"}, {"*.txt"}};
		outname = FileDialogs::save_dialog(_("Save Output..."), outname, filter);
		if(outname.empty()) {
			return false;
		}
		MAIN->getConfig()->getSetting<VarSetting<Glib::ustring>>("outputdir")->setValue(Glib::path_get_dirname(outname));
	}
	std::ofstream file(outname);
	if(!file.is_open()){
		Utils::message_dialog(Gtk::MESSAGE_ERROR, _("Failed to save output"), _("Check that you have writing permissions in the selected folder."));
		return false;
	}
	Glib::ustring txt = m_textBuffer->get_text(false);
	file.write(txt.data(), std::strlen(txt.data()));
	m_textBuffer->set_modified(false);
	return true;
}

bool OutputEditorText::clear()
{
	if(!m_outputBox->get_visible()){
		return true;
	}
	if(m_textBuffer->get_modified()){
		int response = Utils::question_dialog(_("Output not saved"), _("Save output before proceeding?"), Utils::Button::Save|Utils::Button::Discard|Utils::Button::Cancel);
		if(response == Utils::Button::Save){
			if(!save()){
				return false;
			}
		}else if(response != Utils::Button::Discard){
			return false;
		}
	}
	m_textBuffer->begin_not_undoable_action();
	m_textBuffer->set_text("");
	m_textBuffer->end_not_undoable_action();
	m_textBuffer->set_modified(false);
	MAIN->setOutputPaneVisible(false);
	return true;
}

bool OutputEditorText::getModified() const
{
	return m_textBuffer->get_modified();
}

void OutputEditorText::onVisibilityChanged(bool /*visibile*/)
{
	SubstitutionsManager::set_visible(false);
}

void OutputEditorText::setLanguage(const Config::Lang& lang)
{
	m_spell.detach();
	try{
		m_spell.set_language(lang.code.empty() ? Utils::getSpellingLanguage() : lang.code);
		m_spell.attach(*m_textView);
	}catch(const GtkSpell::Error& /*e*/){}
}
