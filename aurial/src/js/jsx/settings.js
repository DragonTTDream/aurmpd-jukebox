import { h, Component } from 'preact';
import Subsonic from '../subsonic'
import {UniqueID} from '../util'
import {Messages} from './app'
import {Prompt} from './common'
import {t, getLanguage, setLanguage, languages} from '../i18n'
// 未识别类型的 WS 消息由 player.js 广播为 "mpdMessage" 事件（见文件头注释），
// 自动启停开关通过它接收 {"type":"autostart", ...} 回包
import {sendCommand} from '../mpdws'

const TEST_UNTESTED = 0;
const TEST_BUSY = 1;
const TEST_SUCCESS = 2;
const TEST_FAILED = 3;

export default class Settings extends Component {

	state = {
		url: this.props.subsonic.url,
		user: this.props.subsonic.user,
		password: '',
		notifications: localStorage.getItem('notifications') === 'true',
		backgroundArt: localStorage.getItem('backgroundArt') === 'true',
		trackBuffer: localStorage.getItem('trackBuffer') || '0',
		testState: TEST_UNTESTED,
		lang: getLanguage(),
		// 开机自启由服务端说了算：supported=null 表示还没问到（先不渲染）
		autostartSupported: null,
		autostartEnabled: false,
		autostartPending: false
	};

	constructor(props, context) {
		super(props, context);

		this.save = this.save.bind(this);
		this.change = this.change.bind(this);
		this.demo = this.demo.bind(this);
		this.test = this.test.bind(this);
		this.changeLanguage = this.changeLanguage.bind(this);
		this.receive = this.receive.bind(this);
		this.toggleAutostart = this.toggleAutostart.bind(this);
		this.queryAutostart = this.queryAutostart.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: ["mpdMessage"]
		});
	}

	componentDidMount() {
		// 设置页在应用启动时就已挂载（标签页只是被 CSS 隐藏），所以这里先查一次，
		// 再在每次点开设置标签时重查，保证看到的是服务端当前值
		this.queryAutostart();
		this.autostartTab = document.querySelector('a.item[data-tab="settings"]');
		if (this.autostartTab) {
			this.autostartTabHandler = () => this.queryAutostart();
			this.autostartTab.addEventListener('click', this.autostartTabHandler);
		}
	}

	componentWillUnmount() {
		this.clearAutostartTimer();
		if (this.autostartTab && this.autostartTabHandler) {
			this.autostartTab.removeEventListener('click', this.autostartTabHandler);
		}
	}

	clearAutostartTimer() {
		if (this.autostartTimer) {
			clearTimeout(this.autostartTimer);
			this.autostartTimer = null;
		}
	}

	queryAutostart() {
		sendCommand('MPD_API_GET_AUTOSTART');
	}

	receive(event) {
		if (event.event == "mpdMessage") this.mpdMessage(event.data);
	}

	// 服务端回包：{"type":"autostart","data":{"supported":bool,"enabled":bool}}
	mpdMessage(msg) {
		if (!msg || msg.type != 'autostart' || !msg.data) return;
		this.clearAutostartTimer();
		// 以服务端回包校正 UI（SET 之后也走这里）
		this.setState({
			autostartSupported: !!msg.data.supported,
			autostartEnabled: !!msg.data.enabled,
			autostartPending: false
		});
	}

	toggleAutostart(e) {
		var next = e.target.checked;
		// 先按点击结果显示，等服务端回包再校正
		this.setState({autostartEnabled: next, autostartPending: true});
		sendCommand('MPD_API_SET_AUTOSTART,' + (next ? 1 : 0));

		// 没有回包/失败时不静默：给可见提示并回到原值
		this.clearAutostartTimer();
		this.autostartTimer = setTimeout(function() {
			this.autostartTimer = null;
			this.setState({autostartEnabled: !next, autostartPending: false});
			Messages.message(this.props.events, t('settings.autostartFailed'), "error", "warning sign");
		}.bind(this), 5000);
	}

	// Language switches instantly (no Save needed) and is persisted by i18n.
	changeLanguage(e) {
		var lang = setLanguage(e.target.value);
		this.setState({lang: lang});
	}

	save(e) {
		e.preventDefault();

		localStorage.setItem('url', this.state.url);
		localStorage.setItem('username', this.state.user);

		if (this.state.password !== '') {
			var salt = UniqueID();
			localStorage.setItem('token', Subsonic.createToken(this.state.password, salt));
			localStorage.setItem('salt', salt);
		}

		localStorage.setItem('notifications', this.state.notifications);
		localStorage.setItem('backgroundArt', this.state.backgroundArt);
		localStorage.setItem('trackBuffer', this.state.trackBuffer);

		Messages.message(this.props.events, t('settings.saved'), "success", "Save");

		// reload app with new settings
		var subsonic = new Subsonic(
			localStorage.getItem('url'),
			localStorage.getItem('username'),
			localStorage.getItem('token'),
			localStorage.getItem('salt'),
			this.props.subsonic.version,
			this.props.subsonic.appName
		);

		// publish new settings to negate need to reload the page - App consumes these
		this.props.events.publish({event: "appSettings",
			data: {
				subsonic: subsonic,
				trackBuffer: localStorage.getItem('trackBuffer')
			}
		});
	}

	demo(e) {
		e.preventDefault();
		this.demoPrompt.show(function(approve) {
			if (!approve) return;

			this.setState({
				url: "http://demo.subsonic.org",
				user: "guest",
				password: "guest"
			});

		}.bind(this));
	}

	test(e) {
		e.preventDefault();

		var salt = UniqueID();

		var subsonic = new Subsonic(
			this.state.url,
			this.state.user,
			Subsonic.createToken(this.state.password, salt),
			salt,
			this.props.subsonic.version,
			this.props.subsonic.appName
		);

		this.setState({testState: TEST_BUSY});

		subsonic.ping({
			success: function(data) {
				if (data.status === "ok") {
					this.setState({testState: TEST_SUCCESS});
					Messages.message(this.props.events, t('settings.testSuccess'), "success", "plug");
				} else {
					console.log(data.error);
					this.setState({testState: TEST_FAILED});
					Messages.message(this.props.events, data.error.message, "error", "plug");
				}
			}.bind(this),
			error: function(err) {
				this.setState({testState: TEST_FAILED});
				Messages.message(this.props.events, t('settings.testFailed', {error: err.message}), "error", "plug");
			}.bind(this)
		});
	}

	change(e) {
		switch (e.target.name) {
			case "url": this.setState({url: e.target.value}); break;
			case "user": this.setState({user: e.target.value}); break;
			case "password": this.setState({password: e.target.value}); break;
			case "notifications": this.setState({notifications: e.target.checked}); break;
			case "backgroundArt": this.setState({backgroundArt: e.target.checked}); break;
			case "trackBuffer": this.setState({trackBuffer: e.target.value}); break;
		}

		this.setState({testState: TEST_UNTESTED});
	}

	render() {
		var testIcon = "circle thin";
		switch (this.state.testState) {
			case TEST_BUSY: testIcon = "loading spinner"; break;
			case TEST_SUCCESS: testIcon = "green checkmark"; break;
			case TEST_FAILED: testIcon = "red warning sign"; break;
			default: testIcon = "circle thin";
		}

		return (
			<div className="ui basic segment">
				<form className="ui form" onSubmit={this.save}>
					<h3 className="ui dividing header">
						{t('settings.language')}
					</h3>
					<div className="field">
						<select name="lang" onChange={this.changeLanguage} value={this.state.lang}>
							{languages.map(function(language) {
								return <option key={language.code} value={language.code}>{language.label}</option>;
							})}
						</select>
					</div>

					<h3 className="ui dividing header">
						{t('settings.connection')}
					</h3>
					<div className="field">
						<label>{t('settings.url')}</label>
						<input name="url" placeholder="http://yourname.subsonic.com" type="text" onChange={this.change} value={this.state.url} />
					</div>
					<div className="two fields">
						<div className="field">
							<label>{t('settings.username')}</label>
							<input name="user" placeholder="username" type="text" onChange={this.change} value={this.state.user} />
						</div>
						<div className="field">
							<label>{t('settings.password')} (<i>{t('settings.passwordHint')}</i>)</label>
							<input name="password" placeholder="password" type="password" onChange={this.change} value={this.state.password} />
						</div>
					</div>

					<h3 className="ui dividing header">
						{t('settings.preferences')}
					</h3>
					<div className="field">
						<label>{t('settings.bufferLabel')}</label>
						<select name="trackBuffer" onChange={this.change} value={this.state.trackBuffer}>
							<option value="0">{t('settings.bufferDisabled')}</option>
							<option value="10">{t('settings.buffer10')}</option>
							<option value="30">{t('settings.buffer30')}</option>
						</select>
					</div>
					<div className="field">
						<div className="ui checkbox">
							<input name="notifications" type="checkbox" onChange={this.change} checked={this.state.notifications}/>
							<label>{t('settings.notifications')}</label>
						</div>
					</div>
					<div className="field">
						<div className="ui checkbox">
							<input name="backgroundArt" type="checkbox" onChange={this.change} checked={this.state.backgroundArt}/>
							<label>{t('settings.backgroundArt')}</label>
						</div>
					</div>
					{/* supported=false（例如 Linux 版）或还没问到回包时，这一项完全不渲染 */}
					{this.state.autostartSupported === true ? (
						<div className="field autostart-field">
							<div className={'ui checkbox' + (this.state.autostartPending ? ' autostart-pending' : '')}>
								<input name="autostart" type="checkbox" onChange={this.toggleAutostart} checked={this.state.autostartEnabled}/>
								<label>{t('settings.autostart')}</label>
							</div>
						</div>
					) : null}

					<div className="ui section divider"></div>

					<button className="ui blue button" type="submit">{t('settings.save')}</button>
					<button className="ui button" onClick={this.demo}>{t('settings.demo')}</button>
					<button className="ui icon button" onClick={this.test}>
						<i className={testIcon + " icon"}></i>
						{t('settings.test')}
					</button>
				</form>

				<Prompt ref={(r) => {this.demoPrompt = r;} } title={t('settings.demoTitle')}
					message={t('settings.demoMessage')}
					ok={t('settings.yes')} cancel={t('settings.no')} icon="red question" />
			</div>
		);
	}
}
