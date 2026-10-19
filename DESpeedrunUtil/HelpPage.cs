using Serilog;

namespace DESpeedrunUtil {
    public partial class HelpPage: Form {

        List<Button> _buttons;
        private readonly Controls.DesruTextScrollBar _helpScrollBar;
        private readonly int _helpTextWidth;

        public HelpPage() {
            InitializeComponent();
            var otherButton = new Button {
                Name = "helpTabButton8", Text = "Other", Tag = "other",
                Location = new Point(helpTabButton6.Left, helpTabButton6.Bottom + 6),
                Size = helpTabButton7.Size, BackColor = helpTabButton7.BackColor,
                ForeColor = helpTabButton7.ForeColor, FlatStyle = helpTabButton7.FlatStyle,
                UseVisualStyleBackColor = false, TabIndex = 19
            };
            otherButton.FlatAppearance.BorderColor = helpTabButton7.FlatAppearance.BorderColor;
            otherButton.Click += TabButton_Click;
            Controls.Add(otherButton);
            _helpTextWidth=helpTextbox.Width;
            _helpScrollBar=new Controls.DesruTextScrollBar(helpTextbox) { Visible=false,Height=helpTextbox.Height,Top=helpTextbox.Top };
            blankPanel.Controls.Add(_helpScrollBar);
            _buttons = new() {
                helpTabButton0,
                helpTabButton1,
                helpTabButton2,
                helpTabButton3,
                helpTabButton4,
                helpTabButton5,
                helpTabButton6,
                helpTabButton7,
                otherButton,
            };
            SetFonts();
            helpTextbox.MouseEnter += (_, _) => {
                if(ContainsFocus && _helpScrollBar.Visible) helpTextbox.Focus();
            };
        }

        private void SetFonts() {
            foreach(Button b in _buttons) b.Font = MainWindow.EternalUIBold;
            closeButton.Font = MainWindow.EternalUIBold;
            helpTextbox.Font = MainWindow.EternalUIRegular;
            titleText.Font = MainWindow.EternalUIRegular;
        }

        private void CloseButton_Click(object sender, EventArgs e) {
            this.Close();
        }

        private void TabButton_Click(object sender, EventArgs e) {
            var button = (Button) sender;
            string tag = (string) button.Tag;
            button.Enabled = false;

            helpTextbox.Text = (tag switch {
                "macro" => Properties.Resources.HelpPage_Macro,
                "hk" => Properties.Resources.HelpPage_Keybinds,
                "res" => Properties.Resources.HelpPage_ResScaling,
                "ver" => Properties.Resources.HelpPage_VersionChanger,
                "option" => Properties.Resources.HelpPage_Options,
                "info" => Properties.Resources.HelpPage_InfoPanel,
                "osd" => Properties.Resources.HelpPage_OSD,
                "trainer" => Properties.Resources.HelpPage_Trainer,
                "other" => Properties.Resources.HelpPage_Other,
                _ => ""
            }).ReplaceLineEndings("\r\n");

            _helpScrollBar.Visible=tag is "trainer" or "osd" or "info" or "other";
            helpTextbox.Width=_helpTextWidth-(_helpScrollBar.Visible?_helpScrollBar.Width+4:0);
            _helpScrollBar.Left=helpTextbox.Right+4;

            helpPageVersionImage.Visible = tag == "ver";
            helpPageOSDImage.Visible = false;
            helpTextbox.SelectionStart = 0;
            helpTextbox.SelectionLength = 0;
            helpTextbox.ScrollToCaret();
            _helpScrollBar.UpdateViewport();
            if(_helpScrollBar.Visible) helpTextbox.Focus();

            foreach(Button b in _buttons)
                if((string) b.Tag != tag)
                    b.Enabled = true;
        }

        private void HelpPage_Load(object sender, EventArgs e) {
            foreach(Button b in _buttons)
                b.Enabled = true;
            blankPanel.Visible = true;
            this.Location = new Point(
                Screen.PrimaryScreen.WorkingArea.Left + (Screen.PrimaryScreen.WorkingArea.Width / 2 - (this.Width / 2)),
                Screen.PrimaryScreen.WorkingArea.Top + (Screen.PrimaryScreen.WorkingArea.Height / 2) - (this.Height / 2));
            Log.Information("Loaded Help Page");
        }

        private void HelpPage_FormClosing(object sender, FormClosingEventArgs e) {
            MainWindow.Instance.ToggleButtonStates("help", true);
        }
    }
}
