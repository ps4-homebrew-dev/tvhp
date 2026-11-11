// The connect view: the saved servers, the add/edit dialog and its focus
// handling, and connecting.
#include <algorithm>
#include <string>

#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include "app.h"
#include "app_internal.h"
#include "icons.h"
#include "keymap.h"

using namespace appdetail;

namespace {
  // The dialog's rows: a label, then its field. The last row is Save (in the
  // label's place) and Cancel (in the field's). Up/down walk the labels, so
  // no text field takes the focus (and brings up the keyboard) until right
  // is pressed; from a field, up/down go to the next field.
  struct FormRow { const char* label; const char* input; };
  constexpr FormRow kFormRows[] = {
    {"lbl-name", "in-name"},
    {"lbl-host", "in-host"},
    {"lbl-port", "in-port"},
    {"lbl-user", "in-user"},
    {"lbl-pass", "in-pass"},
    {"btn-save", "btn-cancel"},
  };
  constexpr int kFormRowCount = (int)(sizeof(kFormRows) / sizeof(kFormRows[0]));
  // The Save/Cancel row: its "input" is a button, not a text field.
  constexpr int kButtonRow = kFormRowCount - 1;
}

void App::RebuildServerRows() {
  server_rows_.clear();
  server_rows_.reserve(config_.Count());
  for (const ServerProfile& p : config_.Servers()) {
    ServerRow row;
    row.name = p.Label();
    row.detail = p.Detail();
    server_rows_.push_back(std::move(row));
  }

  server_count_ = (int)server_rows_.size();
  // sel_server_ == server_count_ is "Add a server".
  sel_server_ = std::clamp(sel_server_, 0, server_count_);
  model_.DirtyVariable("servers");
  model_.DirtyVariable("server_count");
  model_.DirtyVariable("sel_server");
}

void App::LoadFormFromSelection() {
  const ServerProfile* p = config_.At(sel_server_);
  if (p) {
    cfg_name_ = p->name;
    cfg_host_ = p->host;
    cfg_port_ = p->port;
    cfg_user_ = p->user;
    cfg_pass_ = p->pass;
  } else {
    cfg_name_.clear();
    cfg_host_.clear();
    cfg_port_ = "9982";
    cfg_user_.clear();
    cfg_pass_.clear();
  }
  model_.DirtyVariable("name");
  model_.DirtyVariable("host");
  model_.DirtyVariable("port");
  model_.DirtyVariable("user");
  model_.DirtyVariable("pass");
}

ServerProfile App::FormToProfile() const {
  ServerProfile p;
  p.name = cfg_name_;
  p.host = cfg_host_;
  p.port = cfg_port_.empty() ? std::string("9982") : std::string(cfg_port_);
  p.user = cfg_user_;
  p.pass = cfg_pass_;
  return p;
}

// Adding leaves sel_server_ past the end of the list, which is how
// SaveServer() knows to append.
void App::OpenServerDialog(bool add_new) {
  if (add_new)
    sel_server_ = (int)config_.Count();
  else if (!config_.At(sel_server_))
    return; // nothing to edit

  LoadFormFromSelection();
  bind_dialog_ = add_new ? "add" : "edit";
  bind_dialog_title_ = add_new ? "Add server" : "Edit server";
  bind_status_.clear(); // not a connection error in the dialog
  SetZone(Zone::ServerForm);
  model_.DirtyVariable("sel_server");
  model_.DirtyVariable("dialog");
  model_.DirtyVariable("dialog_title");
  model_.DirtyVariable("status");

  if (Rml::Element* el = document_->GetElementById("lbl-name"))
    el->Focus();
}

void App::CloseServerDialog() {
  if (bind_dialog_.empty())
    return;

  bind_dialog_.clear();
  bind_dialog_title_.clear();
  model_.DirtyVariable("dialog");
  model_.DirtyVariable("dialog_title");

  // Adding abandoned: back on a row of the list.
  sel_server_ = std::clamp(sel_server_, 0, server_count_);
  model_.DirtyVariable("sel_server");

  SetZone(Zone::ServerList);
  // Off the text fields, so the keyboard closes.
  if (document_)
    document_->Focus();
}

void App::SaveServer() {
  const ServerProfile p = FormToProfile();
  if (!p.Valid()) {
    bind_status_ = "Enter a hostname first";
    model_.DirtyVariable("status");
    return;
  }

  if (sel_server_ >= (int)config_.Count())
    sel_server_ = config_.Add(p);
  else
    config_.Update(sel_server_, p);

  // The last used server is the last one that connected, not this one.
  config_.Save(config_path_);
  RebuildServerRows();
  LoadFormFromSelection();
  CloseServerDialog();
}

void App::DeleteSelectedServer() {
  const ServerProfile* p = config_.At(sel_server_);
  if (!p)
    return;
  const int index = sel_server_;
  OpenConfirm("Delete " + p->Label() + "?", "Its address and login are forgotten.", "Delete",
    [this, index] {
      config_.Remove(index);
      config_.Save(config_path_);
      sel_server_ = std::min(index, (int)config_.Count());
      RebuildServerRows();
      LoadFormFromSelection();
      ShowToast("Server deleted", icon::kDelete);
    });
}

// Options on a server: what can be done with it; on "Add a server", adding.
void App::OpenServerMenu() {
  const ServerProfile* p = config_.At(sel_server_);
  if (!p) {
    OpenServerDialog(true);
    return;
  }
  OpenContextMenu(p->Label(), {
    {"Connect", nullptr, [this](int) { StartConnect(); }},
    {"Edit server", nullptr, [this](int) { OpenServerDialog(false); }},
    {"Delete server", nullptr, [this](int) { DeleteSelectedServer(); }},
    {"Add a server", nullptr, [this](int) { OpenServerDialog(true); }},
  });
}

void App::ScheduleReconnect(int index, double delay) {
  if (!config_.At(index))
    return;
  auto_index_ = index;
  auto_at_ = Now() + delay;
}

void App::CancelReconnect() {
  if (auto_index_ < 0)
    return;
  auto_index_ = -1;
  auto_tries_ = 0;
  // Without "trying again".
  if (connect_state_ != ConnectState::Busy && !connect_error_.empty())
    Set(bind_status_, Rml::String(connect_error_), "status");
}

void App::StartConnect() {
  if (connect_state_ == ConnectState::Busy)
    return;

  const ServerProfile* p = config_.At(sel_server_);
  if (!p) {
    OpenServerDialog(true); // the "Add a server" row
    return;
  }

  if (connect_thread_.joinable())
    connect_thread_.join();

  bind_status_ = "Connecting to " + p->Label() + "...";
  connecting_index_ = sel_server_;
  model_.DirtyVariable("status");

  const std::string host = p->host;
  const std::string user = p->user;
  const std::string pass = p->pass;
  const int port = p->PortNumber();

  connect_error_.clear();
  connect_state_ = ConnectState::Busy;
  connect_thread_ = std::thread([this, host, port, user, pass]() {
    std::string error;
    if (!client_.Connect(host, port, user, pass, error))
      {
	connect_error_ = error;
	connect_state_ = ConnectState::Failed;
	return;
      }
    client_.StartAsync();
    connect_state_ = ConnectState::Success;
  });
}

void App::PollConnectState() {
  const ConnectState state = connect_state_.load();
  if (state != ConnectState::Success && state != ConnectState::Failed)
    return;

  if (connect_thread_.joinable())
    connect_thread_.join();

  connect_state_ = ConnectState::Idle;

  if (state == ConnectState::Failed) {
    bind_status_ = connect_error_;
    if (auto_index_ >= 0) {
      constexpr int n = sizeof(kReconnectDelaysSec) / sizeof(kReconnectDelaysSec[0]);
      const double delay = kReconnectDelaysSec[std::min(++auto_tries_, n - 1)];
      ScheduleReconnect(auto_index_, delay);
      bind_status_ += ". Trying again in " + std::to_string((int)delay) + " s";
    }
    model_.DirtyVariable("status");
    return;
  }
  auto_index_ = -1;
  auto_tries_ = 0;

  // The next launch starts with it.
  if (connecting_index_ >= 0) {
    config_.SetLastUsed(connecting_index_);
    config_.Save(config_path_);
  }
  if (const ServerProfile* p = config_.At(connecting_index_))
    icons_.Start(&client_, p->host + "_" + std::to_string(p->PortNumber()), p->host, p->user,
                 p->pass);
  SwitchToMain();
}

// The dialog row with the focus, and whether it is on the field (rather
// than the label).
int App::CurrentFormRow(bool* in_input) const
{
  if (in_input)
    *in_input = false;

  Rml::Element* focused = document_->GetFocusLeafNode();
  if (!focused)
    return 0;

  for (int i = 0; i < kFormRowCount; ++i)
    {
      if (Rml::Element* el = document_->GetElementById(kFormRows[i].label))
	if (el == focused || el->GetFirstChild() == focused || focused->GetParentNode() == el)
	  return i;

      if (!kFormRows[i].input)
	continue;

      if (Rml::Element* el = document_->GetElementById(kFormRows[i].input))
	if (el == focused || el->GetFirstChild() == focused || focused->GetParentNode() == el)
	  {
	    if (in_input)
	      *in_input = true;
	    return i;
	  }
    }

  return 0;
}

// The PS4's keyboard appends to a field's text and cannot delete any of it,
// so the text is selected: what is typed replaces it, and dismissing the
// keyboard leaves it as it was.
bool App::EnterFormInput(int row)
{
  if (row < 0 || row >= kButtonRow)
    return false; // the button row has no text field

  Rml::Element* el = document_->GetElementById(kFormRows[row].input);
  if (!el)
    return false;

  el->Focus();
  // After the focus, which may place the caret and drop a selection.
  if (auto* input = dynamic_cast<Rml::ElementFormControlInput*>(el))
    input->Select();
  return true;
}

void App::FocusFormField(int delta)
{
  bool in_input = false;
  const int current = CurrentFormRow(&in_input);
  const int next = std::clamp(current + delta, 0, kFormRowCount - 1);

  // From a field, up/down go to the next field; below the last one is Save.
  if (in_input && current != kButtonRow)
    {
      if (next == current)
	return; // first field and up: stay put
      if (next != kButtonRow)
	{
	  EnterFormInput(next);
	  return;
	}
    }

  if (Rml::Element* el = document_->GetElementById(kFormRows[next].label))
    el->Focus();
}

// Right: from a label into its field. False when the key belongs to the
// focused element (the caret in a field).
bool App::FocusFormInput()
{
  bool in_input = false;
  const int row = CurrentFormRow(&in_input);
  if (in_input || !kFormRows[row].input)
    return false;

  if (row != kButtonRow)
    return EnterFormInput(row);

  // Save -> Cancel: a button, so there is no text to select.
  if (Rml::Element* el = document_->GetElementById(kFormRows[row].input))
    {
      el->Focus();
      return true;
    }
  return false;
}

// Left: from a field back to its label, once the caret is at the start.
bool App::FocusFormLabel()
{
  bool in_input = false;
  const int row = CurrentFormRow(&in_input);
  if (!in_input)
    return false;

  auto* input = dynamic_cast<Rml::ElementFormControlInput*>(
      document_->GetElementById(kFormRows[row].input));
  if (input)
    {
      int sel_start = 0, sel_end = 0;
      Rml::String sel_text;
      input->GetSelection(&sel_start, &sel_end, &sel_text);
      if (sel_start != 0 || sel_end != 0)
	return false; // caret not at the start; let the input handle it
    }

  if (Rml::Element* el = document_->GetElementById(kFormRows[row].label))
    {
      el->Focus();
      return true;
    }
  return false;
}

void App::HandleKeyConnect(Rml::Event& event, int key)
{
  CancelReconnect(); // someone is here to choose
  if (zone_ == Zone::ServerList)
    {
      switch (keymap::Command(key))
	{
	case keymap::Cmd::Up:
	  MoveSelection(-1);
	  break;
	case keymap::Cmd::Down:
	  MoveSelection(+1);
	  break;
	case keymap::Cmd::Options:
	  OpenServerMenu();
	  break;
	case keymap::Cmd::Secondary: // Options > Edit
	  OpenServerDialog(false);
	  break;
	case keymap::Cmd::Remove: // Options > Delete
	  DeleteSelectedServer();
	  break;
	case keymap::Cmd::Ok:
	case keymap::Cmd::PlayPause:
	  StartConnect();
	  break;
	default:
	  return;
	}
      event.StopPropagation();
      return;
    }

  // The dialog has text fields, so Escape alone closes it: Cmd::Back is
  // Backspace too, which the fields need.
  if (key == Rml::Input::KI_ESCAPE)
    {
      CloseServerDialog();
      event.StopPropagation();
      return;
    }

  switch (key)
    {
    case Rml::Input::KI_UP:
      FocusFormField(-1);
      event.StopPropagation();
      break;
    case Rml::Input::KI_DOWN:
      FocusFormField(+1);
      event.StopPropagation();
      break;
    case Rml::Input::KI_RIGHT:
      if (FocusFormInput())
	event.StopPropagation();
      break; // else the caret's
    case Rml::Input::KI_LEFT:
      if (FocusFormLabel())
	event.StopPropagation();
      break;
    default:
      // The rest goes on: Enter clicks the focused button.
      break;
    }
}
