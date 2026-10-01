// Settings page (Clay). Values are stored on the phone; THEME and
// VIBRATE_DISCONNECT are forwarded to the watch with each update.
module.exports = [
  {
    type: 'heading',
    defaultValue: 'Sun Arc'
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Ambient Weather station'
      },
      {
        type: 'text',
        defaultValue: 'Optional. With an Ambient Weather station, the face shows your own ' +
          'temperature, an extra sensor and rain. Create both keys at ambientweather.net ' +
          'under Account &rarr; API Keys. Without keys, the face uses the Open-Meteo ' +
          'forecast for your location.'
      },
      {
        type: 'input',
        messageKey: 'AMBIENT_API_KEY',
        label: 'API key',
        attributes: { placeholder: '64 characters', autocapitalize: 'off', autocorrect: 'off' }
      },
      {
        type: 'input',
        messageKey: 'AMBIENT_APP_KEY',
        label: 'Application key',
        attributes: { placeholder: '64 characters', autocapitalize: 'off', autocorrect: 'off' }
      },
      {
        type: 'select',
        messageKey: 'STATION',
        label: 'Station',
        description: 'Stations appear here after the face has fetched data with your keys.',
        defaultValue: 'auto',
        options: [{ label: 'Most recently reporting', value: 'auto' }]  // filled in at runtime
      },
      {
        type: 'select',
        messageKey: 'EXTRA_SENSOR',
        label: 'Extra sensor',
        description: 'Shown between sunrise and sunset.',
        defaultValue: '1',
        options: [
          { label: 'None', value: 'none' },
          { label: 'Console (indoor)', value: 'in' },
          { label: 'Channel 1', value: '1' },
          { label: 'Channel 2', value: '2' },
          { label: 'Channel 3', value: '3' },
          { label: 'Channel 4', value: '4' },
          { label: 'Channel 5', value: '5' },
          { label: 'Channel 6', value: '6' },
          { label: 'Channel 7', value: '7' },
          { label: 'Channel 8', value: '8' }
        ]
      },
      {
        type: 'select',
        messageKey: 'EXTRA_ICON',
        label: 'Extra sensor icon',
        defaultValue: 'greenhouse',
        options: [
          { label: 'Greenhouse', value: 'greenhouse' },
          { label: 'Home', value: 'home' },
          { label: 'Thermometer', value: 'thermometer' },
          { label: 'Pool', value: 'pool' }
        ]
      }
    ]
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Display'
      },
      {
        type: 'select',
        messageKey: 'THEME',
        label: 'Colors',
        defaultValue: 'navy',
        options: [
          { label: 'Navy', value: 'navy' },
          { label: 'Black', value: 'black' },
          { label: 'Light', value: 'light' }
        ]
      },
      {
        type: 'radiogroup',
        messageKey: 'TEMP_UNITS',
        label: 'Temperature',
        defaultValue: 'F',
        options: [
          { label: 'Fahrenheit', value: 'F' },
          { label: 'Celsius', value: 'C' }
        ]
      }
    ]
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Prices'
      },
      {
        type: 'select',
        messageKey: 'CRYPTO',
        label: 'Crypto (bottom left)',
        defaultValue: 'BTC',
        options: [
          { label: 'Bitcoin', value: 'BTC' },
          { label: 'Ethereum', value: 'ETH' },
          { label: 'Solana', value: 'SOL' },
          { label: 'Hidden', value: 'none' }
        ]
      },
      {
        type: 'select',
        messageKey: 'BTC_CURRENCY',
        label: 'Crypto price in',
        defaultValue: 'USD',
        options: [
          { label: 'US Dollar', value: 'USD' },
          { label: 'Euro', value: 'EUR' },
          { label: 'British Pound', value: 'GBP' },
          { label: 'Canadian Dollar', value: 'CAD' },
          { label: 'Australian Dollar', value: 'AUD' },
          { label: 'Japanese Yen', value: 'JPY' }
        ]
      },
      {
        type: 'select',
        messageKey: 'FX_PAIR',
        label: 'Exchange rate (bottom right)',
        defaultValue: 'EURUSD',
        options: [
          { label: 'EUR / USD', value: 'EURUSD' },
          { label: 'GBP / USD', value: 'GBPUSD' },
          { label: 'USD / JPY', value: 'USDJPY' },
          { label: 'USD / CAD', value: 'USDCAD' },
          { label: 'AUD / USD', value: 'AUDUSD' },
          { label: 'USD / MXN', value: 'USDMXN' },
          { label: 'USD / INR', value: 'USDINR' },
          { label: 'USD / CNY', value: 'USDCNY' },
          { label: 'Hidden', value: 'none' }
        ]
      },
      {
        type: 'text',
        defaultValue: 'Exchange rates update once a day. ' +
          '<a href="https://www.exchangerate-api.com">Rates By Exchange Rate API</a>. ' +
          'Crypto prices from Coinbase; weather from Open-Meteo.'
      }
    ]
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Watch'
      },
      {
        type: 'toggle',
        messageKey: 'VIBRATE_DISCONNECT',
        label: 'Vibrate when the phone disconnects',
        defaultValue: true
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
